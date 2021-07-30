// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_bluetooth_internal_a2dp as a2dp;
use fuchsia_bluetooth::types::PeerId;
use fuchsia_zircon as zx;
use futures::{Future, FutureExt, StreamExt};

/// A client for fuchsia.bluetooth.internal.a2dp.
pub struct Control {
    proxy: Option<a2dp::ControllerProxy>,
}

pub type PauseToken = Option<a2dp::StreamSuspenderProxy>;

impl Control {
    pub fn connect() -> Self {
        let proxy = fuchsia_component::client::connect_to_protocol::<a2dp::ControllerMarker>().ok();
        Self { proxy }
    }

    #[cfg(test)]
    fn from_proxy(proxy: a2dp::ControllerProxy) -> Self {
        Self { proxy: Some(proxy) }
    }

    pub fn pause(
        &self,
        peer_id: Option<PeerId>,
    ) -> impl Future<Output = Result<PauseToken, fidl::Error>> {
        let proxy = match self.proxy.as_ref() {
            None => return futures::future::ok(None).left_future(),
            Some(proxy) => proxy,
        };

        let res = (|| {
            let (suspender_proxy, server_end) = fidl::endpoints::create_proxy()?;
            let mut id = peer_id.map(Into::into);
            Ok((suspender_proxy, proxy.suspend(id.as_mut(), server_end)))
        })();

        async move {
            let (suspender_proxy, suspend_fut) = res?;
            match suspend_fut.await {
                Err(fidl::Error::ClientChannelClosed { status, .. })
                    if status == zx::Status::NOT_FOUND =>
                {
                    // Proxy is not available because we can't connect (delayed).
                    return Ok(None);
                }
                Err(e) => return Err(e),
                Ok(()) => {}
            };

            match suspender_proxy.take_event_stream().next().await {
                Some(Ok(a2dp::StreamSuspenderEvent::OnSuspended {})) => Ok(Some(suspender_proxy)),
                _ => Err(fidl::Error::OutOfRange),
            }
        }
        .right_future()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use async_utils::PollExt;
    use fidl::endpoints::RequestStream;
    use fuchsia_async as fasync;
    use futures::task::Poll;

    #[fuchsia::test]
    fn when_a2dp_not_accessible() {
        let mut exec = fasync::TestExecutor::new().unwrap();
        let control = Control::connect();

        let pause_fut = control.pause(None);
        futures::pin_mut!(pause_fut);

        exec.run_singlethreaded(&mut pause_fut).expect("should be okay");

        let pause_single_fut = control.pause(Some(PeerId(1)));
        futures::pin_mut!(pause_single_fut);

        exec.run_singlethreaded(&mut pause_single_fut).expect("should be okay");
    }

    fn expect_suspend_request(
        exec: &mut fasync::TestExecutor,
        requests: &mut a2dp::ControllerRequestStream,
        expected_peer: Option<PeerId>,
    ) -> (a2dp::ControllerSuspendResponder, a2dp::StreamSuspenderRequestStream) {
        match exec.run_until_stalled(&mut requests.next()) {
            Poll::Ready(Some(Ok(a2dp::ControllerRequest::Suspend {
                responder,
                token,
                peer_id,
            }))) => {
                assert_eq!(peer_id, expected_peer.map(Into::into).map(Box::new));
                (responder, token.into_stream().unwrap())
            }
            x => panic!("Expected a ready controller suspend, got {:?}", x),
        }
    }

    fn expect_suspender_close(
        exec: &mut fasync::TestExecutor,
        requests: &mut a2dp::StreamSuspenderRequestStream,
    ) {
        match exec.run_until_stalled(&mut requests.next()) {
            Poll::Ready(None) => {}
            Poll::Ready(Some(Err(e))) if e.is_closed() => {}
            x => panic!("Expected suspender to be closed, but it wasn't: {:?}", x),
        }
    }

    #[fuchsia::test]
    fn suspend_and_release() {
        let mut exec = fasync::TestExecutor::new().unwrap();
        let (proxy, mut control_requests) =
            fidl::endpoints::create_proxy_and_stream::<a2dp::ControllerMarker>().unwrap();
        let control = Control::from_proxy(proxy);

        let pause_fut = control.pause(Some(PeerId(1)));
        futures::pin_mut!(pause_fut);

        let (responder, mut stream1) =
            expect_suspend_request(&mut exec, &mut control_requests, Some(PeerId(1)));

        responder.send().expect("should send response okay");

        exec.run_until_stalled(&mut pause_fut).expect_pending("shouldn't be done");

        stream1.control_handle().send_on_suspended().expect("should send on suspended event");

        let token = exec.run_until_stalled(&mut pause_fut).expect("done now").expect("token ok");

        // Should be able to have overlapping pauses.
        let pause_fut = control.pause(None);
        futures::pin_mut!(pause_fut);
        let (responder, mut stream2) =
            expect_suspend_request(&mut exec, &mut control_requests, None);
        responder.send().expect("should send response okay");
        stream2.control_handle().send_on_suspended().expect("should send on suspended event");
        let token2 = exec.run_until_stalled(&mut pause_fut).expect("done now").expect("token ok");

        drop(token);

        expect_suspender_close(&mut exec, &mut stream1);

        drop(token2);

        expect_suspender_close(&mut exec, &mut stream2);
    }

    #[fuchsia::test]
    fn suspend_fails() {
        let mut exec = fasync::TestExecutor::new().unwrap();
        let (proxy, mut control_requests) =
            fidl::endpoints::create_proxy_and_stream::<a2dp::ControllerMarker>().unwrap();
        let control = Control::from_proxy(proxy);

        let pause_fut = control.pause(Some(PeerId(1)));
        futures::pin_mut!(pause_fut);

        let (responder, _stream1) =
            expect_suspend_request(&mut exec, &mut control_requests, Some(PeerId(1)));

        drop(responder);
        drop(control_requests);

        exec.run_singlethreaded(&mut pause_fut).expect_err("token error");
    }

    #[fuchsia::test]
    fn proxy_is_closed_before_suspend_event() {
        let mut exec = fasync::TestExecutor::new().unwrap();
        let (proxy, mut control_requests) =
            fidl::endpoints::create_proxy_and_stream::<a2dp::ControllerMarker>().unwrap();
        let control = Control::from_proxy(proxy);

        let pause_fut = control.pause(Some(PeerId(1)));
        futures::pin_mut!(pause_fut);

        let (responder, stream1) =
            expect_suspend_request(&mut exec, &mut control_requests, Some(PeerId(1)));

        responder.send().expect("should send response okay");

        exec.run_until_stalled(&mut pause_fut).expect_pending("shouldn't be done");

        drop(stream1);

        exec.run_singlethreaded(&mut pause_fut).expect_err("token error");
    }
}
