// Copyright 2020 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package stack

import (
	"encoding/binary"
	"fmt"
	"sync"
	"time"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/hash/jenkins"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcpconntrack"
)

// Connection tracking is used to track and manipulate packets for NAT rules.
// The connection is created for a packet if it does not exist. Every
// connection contains two tuples (original and reply). The tuples are
// manipulated if there is a matching NAT rule. The packet is modified by
// looking at the tuples in each hook.
//
// Currently, only TCP tracking is supported.

// Our hash table has 16K buckets.
const numBuckets = 1 << 14

// Direction of the tuple.
type direction int

const (
	dirOriginal direction = iota
	dirReply
)

// Manipulation type for the connection.
// TODO(gvisor.dev/issue/5696): Define this as a bit set and support SNAT and
// DNAT at the same time.
type manipType int

const (
	manipNone manipType = iota
	manipSource
	manipDestination
)

// tuple holds a connection's identifying and manipulating data in one
// direction. It is immutable.
//
// +stateify savable
type tuple struct {
	// tupleEntry is used to build an intrusive list of tuples.
	tupleEntry

	tupleID

	// conn is the connection tracking entry this tuple belongs to.
	conn *conn

	// direction is the direction of the tuple.
	direction direction
}

// tupleID uniquely identifies a connection in one direction. It currently
// contains enough information to distinguish between any TCP or UDP
// connection, and will need to be extended to support other protocols.
//
// +stateify savable
type tupleID struct {
	srcAddr    tcpip.Address
	srcPort    uint16
	dstAddr    tcpip.Address
	dstPort    uint16
	transProto tcpip.TransportProtocolNumber
	netProto   tcpip.NetworkProtocolNumber
}

// reply creates the reply tupleID.
func (ti tupleID) reply() tupleID {
	return tupleID{
		srcAddr:    ti.dstAddr,
		srcPort:    ti.dstPort,
		dstAddr:    ti.srcAddr,
		dstPort:    ti.srcPort,
		transProto: ti.transProto,
		netProto:   ti.netProto,
	}
}

// conn is a tracked connection.
//
// +stateify savable
type conn struct {
	// original is the tuple in original direction. It is immutable.
	original tuple

	// reply is the tuple in reply direction. It is immutable.
	reply tuple

	// manip indicates if the packet should be manipulated. It is immutable.
	// TODO(gvisor.dev/issue/5696): Support updating manipulation type.
	manip manipType

	mu sync.RWMutex `state:"nosave"`
	// tcb is TCB control block. It is used to keep track of states
	// of tcp connection.
	//
	// +checklocks:mu
	tcb tcpconntrack.TCB
	// lastUsed is the last time the connection saw a relevant packet, and
	// is updated by each packet on the connection.
	//
	// TODO(gvisor.dev/issue/5939): do not use the ambient clock.
	//
	// +checklocks:mu
	lastUsed time.Time `state:".(unixTime)"`
}

// newConn creates new connection.
func newConn(orig, reply tupleID, manip manipType) *conn {
	conn := conn{
		manip:    manip,
		lastUsed: time.Now(),
	}
	conn.original = tuple{conn: &conn, tupleID: orig}
	conn.reply = tuple{conn: &conn, tupleID: reply, direction: dirReply}
	return &conn
}

// timedOut returns whether the connection timed out based on its state.
func (cn *conn) timedOut(now time.Time) bool {
	const establishedTimeout = 5 * 24 * time.Hour
	const defaultTimeout = 120 * time.Second
	cn.mu.RLock()
	defer cn.mu.RUnlock()
	if cn.tcb.State() == tcpconntrack.ResultAlive {
		// Use the same default as Linux, which doesn't delete
		// established connections for 5(!) days.
		return now.Sub(cn.lastUsed) > establishedTimeout
	}
	// Use the same default as Linux, which lets connections in most states
	// other than established remain for <= 120 seconds.
	return now.Sub(cn.lastUsed) > defaultTimeout
}

// update the connection tracking state.
//
// TODO(https://gvisor.dev/issue/6590): annotate r/w locking requirements.
// +checklocks:cn.mu
func (cn *conn) updateLocked(pkt *PacketBuffer, dir direction) {
	if pkt.TransportProtocolNumber != header.TCPProtocolNumber {
		return
	}

	tcpHeader := header.TCP(pkt.TransportHeader().View())

	// Update the state of tcb. tcb assumes it's always initialized on the
	// client. However, we only need to know whether the connection is
	// established or not, so the client/server distinction isn't important.
	if cn.tcb.IsEmpty() {
		cn.tcb.Init(tcpHeader)
		return
	}

	switch dir {
	case dirOriginal:
		cn.tcb.UpdateStateOutbound(tcpHeader)
	case dirReply:
		cn.tcb.UpdateStateInbound(tcpHeader)
	default:
		panic(fmt.Sprintf("unhandled dir = %d", dir))
	}
}

// ConnTrack tracks all connections created for NAT rules. Most users are
// expected to only call handlePacket, insertRedirectConn, and maybeInsertNoop.
//
// ConnTrack keeps all connections in a slice of buckets, each of which holds a
// linked list of tuples. This gives us some desirable properties:
// - Each bucket has its own lock, lessening lock contention.
// - The slice is large enough that lists stay short (<10 elements on average).
//   Thus traversal is fast.
// - During linked list traversal we reap expired connections. This amortizes
//   the cost of reaping them and makes reapUnused faster.
//
// Locks are ordered by their location in the buckets slice. That is, a
// goroutine that locks buckets[i] can only lock buckets[j] s.t. i < j.
//
// +stateify savable
type ConnTrack struct {
	// seed is a one-time random value initialized at stack startup
	// and is used in the calculation of hash keys for the list of buckets.
	// It is immutable.
	seed uint32

	mu sync.RWMutex `state:"nosave"`
	// mu protects the buckets slice, but not buckets' contents. Only take
	// the write lock if you are modifying the slice or saving for S/R.
	//
	// +checklocks:mu
	buckets []bucket
}

// +stateify savable
type bucket struct {
	mu sync.RWMutex `state:"nosave"`
	// +checklocks:mu
	tuples tupleList
}

func getTransportHeader(pkt *PacketBuffer) (header.ChecksummableTransport, bool) {
	switch pkt.TransportProtocolNumber {
	case header.TCPProtocolNumber:
		if tcpHeader := header.TCP(pkt.TransportHeader().View()); len(tcpHeader) >= header.TCPMinimumSize {
			return tcpHeader, true
		}
	case header.UDPProtocolNumber:
		if udpHeader := header.UDP(pkt.TransportHeader().View()); len(udpHeader) >= header.UDPMinimumSize {
			return udpHeader, true
		}
	}

	return nil, false
}

// packetToTupleID converts packet to a tuple ID. It fails when pkt lacks a valid
// TCP header.
//
// Preconditions: pkt.NetworkHeader() is valid.
func packetToTupleID(pkt *PacketBuffer) (tupleID, tcpip.Error) {
	netHeader := pkt.Network()
	transportHeader, ok := getTransportHeader(pkt)
	if !ok {
		return tupleID{}, &tcpip.ErrUnknownProtocol{}
	}

	return tupleID{
		srcAddr:    netHeader.SourceAddress(),
		srcPort:    transportHeader.SourcePort(),
		dstAddr:    netHeader.DestinationAddress(),
		dstPort:    transportHeader.DestinationPort(),
		transProto: pkt.TransportProtocolNumber,
		netProto:   pkt.NetworkProtocolNumber,
	}, nil
}

func (ct *ConnTrack) init() {
	ct.mu.Lock()
	defer ct.mu.Unlock()
	ct.buckets = make([]bucket, numBuckets)
}

// connFor gets the conn for pkt if it exists, or returns nil
// if it does not. It returns an error when pkt does not contain a valid TCP
// header.
// TODO(gvisor.dev/issue/6168): Support UDP.
func (ct *ConnTrack) connFor(pkt *PacketBuffer) (*conn, direction) {
	tid, err := packetToTupleID(pkt)
	if err != nil {
		return nil, dirOriginal
	}
	return ct.connForTID(tid)
}

func (ct *ConnTrack) connForTID(tid tupleID) (*conn, direction) {
	bktID := ct.bucket(tid)
	now := time.Now()

	ct.mu.RLock()
	bkt := &ct.buckets[bktID]
	ct.mu.RUnlock()

	bkt.mu.RLock()
	defer bkt.mu.RUnlock()
	for other := bkt.tuples.Front(); other != nil; other = other.Next() {
		if tid == other.tupleID && !other.conn.timedOut(now) {
			return other.conn, other.direction
		}
	}

	return nil, dirOriginal
}

func (ct *ConnTrack) insertRedirectConn(pkt *PacketBuffer, hook Hook, port uint16, address tcpip.Address) *conn {
	tid, err := packetToTupleID(pkt)
	if err != nil {
		return nil
	}
	if hook != Prerouting && hook != Output {
		return nil
	}

	replyTID := tid.reply()
	replyTID.srcAddr = address
	replyTID.srcPort = port

	conn, _ := ct.connForTID(tid)
	if conn != nil {
		// The connection is already tracked.
		// TODO(gvisor.dev/issue/5696): Support updating an existing connection.
		return nil
	}
	conn = newConn(tid, replyTID, manipDestination)
	ct.insertConn(conn)
	return conn
}

func (ct *ConnTrack) insertSNATConn(pkt *PacketBuffer, hook Hook, port uint16, address tcpip.Address) *conn {
	tid, err := packetToTupleID(pkt)
	if err != nil {
		return nil
	}
	if hook != Input && hook != Postrouting {
		return nil
	}

	replyTID := tid.reply()
	replyTID.dstAddr = address
	replyTID.dstPort = port

	conn, _ := ct.connForTID(tid)
	if conn != nil {
		// The connection is already tracked.
		// TODO(gvisor.dev/issue/5696): Support updating an existing connection.
		return nil
	}
	conn = newConn(tid, replyTID, manipSource)
	ct.insertConn(conn)
	return conn
}

// insertConn inserts conn into the appropriate table bucket.
func (ct *ConnTrack) insertConn(conn *conn) {
	tupleBktID := ct.bucket(conn.original.tupleID)
	replyBktID := ct.bucket(conn.reply.tupleID)

	ct.mu.RLock()
	defer ct.mu.RUnlock()

	tupleBkt := &ct.buckets[tupleBktID]
	if tupleBktID == replyBktID {
		// Both tuples are in the same bucket.
		tupleBkt.mu.Lock()
		defer tupleBkt.mu.Unlock()
		insertConn(tupleBkt, tupleBkt, conn)
		return
	}

	// Lock the buckets in the correct order.
	replyBkt := &ct.buckets[replyBktID]
	if tupleBktID < replyBktID {
		tupleBkt.mu.Lock()
		defer tupleBkt.mu.Unlock()
		replyBkt.mu.Lock()
		defer replyBkt.mu.Unlock()
	} else {
		replyBkt.mu.Lock()
		defer replyBkt.mu.Unlock()
		tupleBkt.mu.Lock()
		defer tupleBkt.mu.Unlock()
	}
	insertConn(tupleBkt, replyBkt, conn)
}

// TODO(https://gvisor.dev/issue/6590): annotate r/w locking requirements.
// +checklocks:tupleBkt.mu
// +checklocks:replyBkt.mu
func insertConn(tupleBkt *bucket, replyBkt *bucket, conn *conn) {
	// Now that we hold the locks, ensure the tuple hasn't been inserted by
	// another thread.
	// TODO(gvisor.dev/issue/5773): Should check conn.reply.tupleID, too?
	alreadyInserted := false
	for other := tupleBkt.tuples.Front(); other != nil; other = other.Next() {
		if other.tupleID == conn.original.tupleID {
			alreadyInserted = true
			break
		}
	}

	if !alreadyInserted {
		// Add the tuple to the map.
		tupleBkt.tuples.PushFront(&conn.original)
		replyBkt.tuples.PushFront(&conn.reply)
	}
}

func (cn *conn) handlePacket(pkt *PacketBuffer, hook Hook, dir direction, r *Route) {
	if pkt.NatDone {
		return
	}

	transportHeader, ok := getTransportHeader(pkt)
	if !ok {
		return
	}

	netHeader := pkt.Network()

	// TODO(gvisor.dev/issue/5748): TCP checksums on inbound packets should be
	// validated if checksum offloading is off. It may require IP defrag if the
	// packets are fragmented.

	var newAddr tcpip.Address
	var newPort uint16

	updateSRCFields := false

	switch hook {
	case Prerouting, Output:
		if cn.manip == manipDestination && dir == dirOriginal {
			newPort = cn.reply.srcPort
			newAddr = cn.reply.srcAddr
			pkt.NatDone = true
		} else if cn.manip == manipSource && dir == dirReply {
			newPort = cn.original.srcPort
			newAddr = cn.original.srcAddr
			pkt.NatDone = true
		}
	case Input, Postrouting:
		if cn.manip == manipSource && dir == dirOriginal {
			newPort = cn.reply.dstPort
			newAddr = cn.reply.dstAddr
			updateSRCFields = true
			pkt.NatDone = true
		} else if cn.manip == manipDestination && dir == dirReply {
			newPort = cn.original.dstPort
			newAddr = cn.original.dstAddr
			updateSRCFields = true
			pkt.NatDone = true
		}
	default:
		panic(fmt.Sprintf("unrecognized hook = %s", hook))
	}

	if !pkt.NatDone {
		return
	}

	fullChecksum := false
	updatePseudoHeader := false
	switch hook {
	case Prerouting:
		// Packet came from outside the stack so it must have a checksum set
		// already.
		fullChecksum = true
		updatePseudoHeader = true
	case Input:
	case Output, Postrouting:
		// Calculate the TCP checksum and set it.
		if pkt.TransportProtocolNumber == header.TCPProtocolNumber && pkt.GSOOptions.Type != GSONone && pkt.GSOOptions.NeedsCsum {
			updatePseudoHeader = true
		} else if r.RequiresTXTransportChecksum() {
			fullChecksum = true
			updatePseudoHeader = true
		}
	default:
		panic(fmt.Sprintf("unrecognized hook = %s", hook))
	}

	rewritePacket(
		netHeader,
		transportHeader,
		updateSRCFields,
		fullChecksum,
		updatePseudoHeader,
		newPort,
		newAddr,
	)

	// Update the state of tcb.
	cn.mu.Lock()
	defer cn.mu.Unlock()

	// Mark the connection as having been used recently so it isn't reaped.
	cn.lastUsed = time.Now()
	// Update connection state.
	cn.updateLocked(pkt, dir)
}

// maybeInsertNoop tries to insert a no-op connection entry to keep connections
// from getting clobbered when replies arrive. It only inserts if there isn't
// already a connection for pkt.
//
// This should be called after traversing iptables rules only, to ensure that
// pkt.NatDone is set correctly.
func (ct *ConnTrack) maybeInsertNoop(pkt *PacketBuffer) {
	// If there were a rule applying to this packet, it would be marked
	// with NatDone.
	if pkt.NatDone {
		return
	}

	switch pkt.TransportProtocolNumber {
	case header.TCPProtocolNumber, header.UDPProtocolNumber:
	default:
		// TODO(https://gvisor.dev/issue/5915): Track ICMP and other trackable
		// connections.
		return
	}

	// This is the first packet we're seeing for the TCP connection. Insert
	// the noop entry (an identity mapping) so that the response doesn't
	// get NATed, breaking the connection.
	tid, err := packetToTupleID(pkt)
	if err != nil {
		return
	}
	conn := newConn(tid, tid.reply(), manipNone)
	ct.insertConn(conn)
	conn.mu.Lock()
	defer conn.mu.Unlock()
	conn.updateLocked(pkt, dirOriginal)
}

// bucket gets the conntrack bucket for a tupleID.
func (ct *ConnTrack) bucket(id tupleID) int {
	h := jenkins.Sum32(ct.seed)
	h.Write([]byte(id.srcAddr))
	h.Write([]byte(id.dstAddr))
	shortBuf := make([]byte, 2)
	binary.LittleEndian.PutUint16(shortBuf, id.srcPort)
	h.Write([]byte(shortBuf))
	binary.LittleEndian.PutUint16(shortBuf, id.dstPort)
	h.Write([]byte(shortBuf))
	binary.LittleEndian.PutUint16(shortBuf, uint16(id.transProto))
	h.Write([]byte(shortBuf))
	binary.LittleEndian.PutUint16(shortBuf, uint16(id.netProto))
	h.Write([]byte(shortBuf))
	ct.mu.RLock()
	defer ct.mu.RUnlock()
	return int(h.Sum32()) % len(ct.buckets)
}

// reapUnused deletes timed out entries from the conntrack map. The rules for
// reaping are:
// - Most reaping occurs in connFor, which is called on each packet. connFor
//   cleans up the bucket the packet's connection maps to. Thus calls to
//   reapUnused should be fast.
// - Each call to reapUnused traverses a fraction of the conntrack table.
//   Specifically, it traverses len(ct.buckets)/fractionPerReaping.
// - After reaping, reapUnused decides when it should next run based on the
//   ratio of expired connections to examined connections. If the ratio is
//   greater than maxExpiredPct, it schedules the next run quickly. Otherwise it
//   slightly increases the interval between runs.
// - maxFullTraversal caps the time it takes to traverse the entire table.
//
// reapUnused returns the next bucket that should be checked and the time after
// which it should be called again.
func (ct *ConnTrack) reapUnused(start int, prevInterval time.Duration) (int, time.Duration) {
	const fractionPerReaping = 128
	const maxExpiredPct = 50
	const maxFullTraversal = 60 * time.Second
	const minInterval = 10 * time.Millisecond
	const maxInterval = maxFullTraversal / fractionPerReaping

	now := time.Now()
	checked := 0
	expired := 0
	var idx int
	ct.mu.RLock()
	defer ct.mu.RUnlock()
	for i := 0; i < len(ct.buckets)/fractionPerReaping; i++ {
		idx = (i + start) % len(ct.buckets)
		bkt := &ct.buckets[idx]
		bkt.mu.Lock()
		for tuple := bkt.tuples.Front(); tuple != nil; tuple = tuple.Next() {
			checked++
			if ct.reapTupleLocked(tuple, idx, bkt, now) {
				expired++
			}
		}
		bkt.mu.Unlock()
	}
	// We already checked buckets[idx].
	idx++

	// If half or more of the connections are expired, the table has gotten
	// stale. Reschedule quickly.
	expiredPct := 0
	if checked != 0 {
		expiredPct = expired * 100 / checked
	}
	if expiredPct > maxExpiredPct {
		return idx, minInterval
	}
	if interval := prevInterval + minInterval; interval <= maxInterval {
		// Increment the interval between runs.
		return idx, interval
	}
	// We've hit the maximum interval.
	return idx, maxInterval
}

// reapTupleLocked tries to remove tuple and its reply from the table. It
// returns whether the tuple's connection has timed out.
//
// Precondition: ct.mu is read locked and bkt.mu is write locked.
// TODO(https://gvisor.dev/issue/6590): annotate r/w locking requirements.
// +checklocks:ct.mu
// +checklocks:bkt.mu
func (ct *ConnTrack) reapTupleLocked(tuple *tuple, bktID int, bkt *bucket, now time.Time) bool {
	if !tuple.conn.timedOut(now) {
		return false
	}

	// To maintain lock order, we can only reap these tuples if the reply
	// appears later in the table.
	replyBktID := ct.bucket(tuple.reply())
	if bktID > replyBktID {
		return true
	}

	// Don't re-lock if both tuples are in the same bucket.
	if bktID != replyBktID {
		replyBkt := &ct.buckets[replyBktID]
		replyBkt.mu.Lock()
		removeConnFromBucket(replyBkt, tuple)
		replyBkt.mu.Unlock()
	} else {
		removeConnFromBucket(bkt, tuple)
	}

	// We have the buckets locked and can remove both tuples.
	bkt.tuples.Remove(tuple)
	return true
}

// TODO(https://gvisor.dev/issue/6590): annotate r/w locking requirements.
// +checklocks:b.mu
func removeConnFromBucket(b *bucket, tuple *tuple) {
	if tuple.direction == dirOriginal {
		b.tuples.Remove(&tuple.conn.reply)
	} else {
		b.tuples.Remove(&tuple.conn.original)
	}
}

func (ct *ConnTrack) originalDst(epID TransportEndpointID, netProto tcpip.NetworkProtocolNumber, transProto tcpip.TransportProtocolNumber) (tcpip.Address, uint16, tcpip.Error) {
	// Lookup the connection. The reply's original destination
	// describes the original address.
	tid := tupleID{
		srcAddr:    epID.LocalAddress,
		srcPort:    epID.LocalPort,
		dstAddr:    epID.RemoteAddress,
		dstPort:    epID.RemotePort,
		transProto: transProto,
		netProto:   netProto,
	}
	conn, _ := ct.connForTID(tid)
	if conn == nil {
		// Not a tracked connection.
		return "", 0, &tcpip.ErrNotConnected{}
	} else if conn.manip != manipDestination {
		// Unmanipulated destination.
		return "", 0, &tcpip.ErrInvalidOptionValue{}
	}

	return conn.original.dstAddr, conn.original.dstPort, nil
}
