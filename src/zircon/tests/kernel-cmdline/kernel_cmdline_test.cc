// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string_view>

#include <zxtest/zxtest.h>

#include "lib/cmdline.h"

namespace {

// Print the command line in hex (for debugging test failures).
void PrintHex(const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    printf("%02hhx ", data[i]);
  }
}

// "Base case" for EqualsOffset parameter pack template.
//
// See Equals template below.
bool EqualsOffset(const char* data, size_t size, size_t offset, const char* value) {
  if (offset >= size) {
    return false;
  }
  if (strcmp(data + offset, value)) {
    return false;
  }
  return true;
}

// See Equals template below.
template <typename... Rest>
bool EqualsOffset(const char* data, size_t size, size_t offset, const char* value, Rest... rest) {
  if (!EqualsOffset(data, size, offset, value)) {
    return false;
  }
  // Step over the value and the '\0'.
  return EqualsOffset(data, size, offset + strlen(value) + 1, rest...);
}

// Compares |data|, a sequence of \0-terminated strings followed by a final \0, with the |rest|
// args.
//
// Example:
//   assert(Equals(c, "k1=v1", "k2=v2", "k3=v3"));
template <typename... Rest>
bool Equals(Cmdline* c, Rest... rest) {
  if (!EqualsOffset(c->data(), c->size(), 0, rest...)) {
    printf("Cmdline contains: [ ");
    PrintHex(c->data(), c->size());
    printf("]\n");
    return false;
  }
  return true;
}

TEST(KernelCmdlineTest, InitialState) {
  auto c = std::make_unique<Cmdline>();
  ASSERT_EQ(1u, c->size());
  ASSERT_EQ('\0', c->data()[0]);
}

TEST(KernelCmdlineTest, AppendBasic) {
  // nullptr
  auto c = std::make_unique<Cmdline>();
  c->Append(nullptr);
  EXPECT_TRUE(Equals(c.get(), ""));
  EXPECT_EQ(1u, c->size());

  // empty string
  c = std::make_unique<Cmdline>();
  c->Append("");
  EXPECT_TRUE(Equals(c.get(), ""));
  EXPECT_EQ(1u, c->size());

  // single whitespace
  c = std::make_unique<Cmdline>();
  c->Append(" ");
  EXPECT_TRUE(Equals(c.get(), ""));
  EXPECT_EQ(1u, c->size());

  // multiple whitespace
  c = std::make_unique<Cmdline>();
  c->Append("    ");
  EXPECT_TRUE(Equals(c.get(), ""));
  EXPECT_EQ(1u, c->size());

  // key only
  c = std::make_unique<Cmdline>();
  c->Append("k");
  EXPECT_TRUE(Equals(c.get(), "k="));
  EXPECT_EQ(2 + strlen(c->data()), c->size());

  // whitespace before key
  c = std::make_unique<Cmdline>();
  c->Append(" k");
  EXPECT_TRUE(Equals(c.get(), "k="));
  EXPECT_EQ(2 + strlen(c->data()), c->size());

  // key equals
  c = std::make_unique<Cmdline>();
  c->Append("k=");
  EXPECT_TRUE(Equals(c.get(), "k="));
  EXPECT_EQ(2 + strlen(c->data()), c->size());

  // two keys
  c = std::make_unique<Cmdline>();
  c->Append("k1 k2");
  EXPECT_TRUE(Equals(c.get(), "k1=", "k2="));

  // white space collapsing
  c = std::make_unique<Cmdline>();
  c->Append("  k1    k2   ");
  EXPECT_TRUE(Equals(c.get(), "k1=", "k2="));

  // key equals value
  c = std::make_unique<Cmdline>();
  c->Append(" k1=hello  k2=world   ");
  EXPECT_TRUE(Equals(c.get(), "k1=hello", "k2=world"));

  // illegal chars become dot
  c = std::make_unique<Cmdline>();
  c->Append(
      " k1=foo  k2=red"
      "\xf8"
      "\x07"
      "blue");
  EXPECT_TRUE(Equals(c.get(), "k1=foo", "k2=red..blue"));
}

TEST(KernelCmdlineTest, OverflowByALot) {
  auto c = std::make_unique<Cmdline>();
  ASSERT_DEATH(([&c]() {
    constexpr char kPattern[] = "abcdefg";
    for (size_t j = 0; j < Cmdline::kCmdlineMax; ++j) {
      c->Append(kPattern);
    }
  }));
}

TEST(KernelCmdlineTest, OverflowExact) {
  // Maximum is 'aaaaa...aaaaa' followed by '=\0\0'. So the longest string that
  // can be added is 3 less than the max. Allocate a buffer 2 less than the max,
  // so that we can \0 terminate this string, that has a strlen() of 3 less than
  // the max.
  auto c = std::make_unique<Cmdline>();
  char data[Cmdline::kCmdlineMax - 2];
  memset(data, 'a', Cmdline::kCmdlineMax - 3);
  data[Cmdline::kCmdlineMax - 3] = 0;
  EXPECT_EQ(strlen(data), Cmdline::kCmdlineMax - 3);
  c->Append(data);
  EXPECT_EQ(c->size(), Cmdline::kCmdlineMax);

  // Adding anything now should abort.
  ASSERT_DEATH(([&c]() { c->Append("b"); }));

  // However, adding "b" actually adds "b=\0" to the total length, so test 2
  // fewer than above as well for the "full" starting amount.
  auto c2 = std::make_unique<Cmdline>();
  memset(data, 'a', Cmdline::kCmdlineMax - 5);
  data[Cmdline::kCmdlineMax - 5] = 0;
  EXPECT_EQ(strlen(data), Cmdline::kCmdlineMax - 5);
  c2->Append(data);
  EXPECT_EQ(c2->size(), Cmdline::kCmdlineMax - 2);

  ASSERT_DEATH(([&c2]() { c2->Append("b"); }));

  // Finally, confirm that one fewer doesn't fail.
  auto c3 = std::make_unique<Cmdline>();
  memset(data, 'a', Cmdline::kCmdlineMax - 6);
  data[Cmdline::kCmdlineMax - 6] = 0;
  EXPECT_EQ(strlen(data), Cmdline::kCmdlineMax - 6);
  c3->Append(data);
  EXPECT_EQ(c3->size(), Cmdline::kCmdlineMax - 3);

  // Shouldn't crash, cmdline is now full.
  c3->Append("b");
  EXPECT_EQ(c3->size(), Cmdline::kCmdlineMax);
}

TEST(KernelCmdlineTest, GetString) {
  auto c = std::make_unique<Cmdline>();
  EXPECT_EQ(nullptr, c->GetString("k1"));
  EXPECT_EQ(nullptr, c->GetString(""));
  EXPECT_EQ(c->data(), c->GetString(nullptr));

  c->Append("k1=red k2=blue k1=green");
  EXPECT_TRUE(!strcmp(c->GetString("k1"), "green"));
  EXPECT_TRUE(!strcmp(c->GetString("k2"), "blue"));
  EXPECT_EQ(nullptr, c->GetString(""));
  EXPECT_EQ(c->data(), c->GetString(nullptr));
}

TEST(KernelCmdlineTest, LaterOverride) {
  auto c = std::make_unique<Cmdline>();
  c->Append("k1 k2= k1=42");
  EXPECT_TRUE(strcmp(c->GetString("k1"), "42") == 0);
  EXPECT_TRUE(strcmp(c->GetString("k2"), "") == 0);

  c->Append("k1=stuff");
  EXPECT_TRUE(strcmp(c->GetString("k1"), "stuff") == 0);

  c->Append("k1=zip k1=zap");
  EXPECT_TRUE(strcmp(c->GetString("k1"), "zap") == 0);

  c->Append("k1");
  EXPECT_TRUE(strcmp(c->GetString("k1"), "") == 0);
}

TEST(KernelCmdlineTest, AlmostMaximumExpansion) {
  auto c = std::make_unique<Cmdline>();

  static_assert(Cmdline::kCmdlineMax == 4096, "1365 below needs to be updated");

  // Appending "a " turns into "a=\0" in the buffer, for 4095 bytes, leaving one
  // byte for the additional terminator.
  for (size_t i = 0; i < 1365; ++i) {
    c->Append("a ");
  }

  // One more should panic though.
  ASSERT_DEATH(([&c]() { c->Append("a "); }));
}

TEST(KernelCmdlineTest, MaximumExpansion) {
  auto c = std::make_unique<Cmdline>();

  static_assert(Cmdline::kCmdlineMax == 4096, "1364 below needs to be updated");

  // AlmostMaximumExpansion was the first attempt at finding the correct maximal
  // input, but, as a fuzzer found, there's a larger case. See fxbug.dev/47006
  // for repro.
  //
  // Consider if the limit were 10 (rather than 4096), then an analogous
  // 'longest' input would be "a a a " which would expand to "a=0a=0a=00" (using
  // 0 instead of \0 for readability), so 6->10 (including terminator).
  //
  // But, because the end of the string also counts as the end of a variable,
  // these 6 characters as input instead "a a aa" result in "a=0a=0aa=00" for an
  // expansion of 6->11. (This is sort of like getting a bonus 7th character in
  // the input due to null termination, rather than needing to have the trailing
  // space to end the variable.)
  //
  // Attempting to use this "trick" twice results in fewer \0 being inserted, so
  // doesn't further lengthen the string.
  //
  // (The specifics aren't particularly important as the kernel is already going
  // to panic if the limit is exceeded, but it's necessary to get the edges
  // right for this test and the fuzzer test.)

  for (size_t i = 0; i < 1364; ++i) {
    c->Append("a ");
  }

  // One more should panic though.
  ASSERT_DEATH(([&c]() { c->Append("aa"); }));
}

TEST(KernelCmdlineTest, ProcessRamReservations) {
  auto c = std::make_unique<Cmdline>();
  static constexpr const char* kTestVector =
      "kernel.Ram.reserve.t0=0x2000,0xXXXXXXXXXXXXXXXX "   // Wrong argument name
      "kernel.ram.reserve.t1=0x20000xXXXXXXXXXXXXXXXX "    // Missing ','
      "kernel.ram.reserve.t2= "                            // Missing args
      "kernel.ram.reserve.t3=0x300G,0xXXXXXXXXXXXXXXXX "   // Bad size
      "kernel.ram.reserve.t4=0x3000,0xXXXXXXXxXXXXXXXX "   // Bad dynamic allocation placeholder
      "kernel.ram.reserve.t5=0x3000,0xXXXXXXXXXXXXXXX "    // Short dynamic allocation placeholder
      "kernel.ram.reserve.t6=0x3000,0xXXXXXXXXXXXXXXXXX "  // Long dynamic allocation placeholder
      "kernel.ram.reserve.t7=0x1000,0xXXXXXXXXXXXXXXXX "   // Good dynamic allocation
      "kernel.ram.reserve.t8=0x2000,0xXXXXXXXXXXXXXXXX "   // Duplicate name, should be erased
      "kernel.ram.reserve.t8=0x3000,0xXXXXXXXXXXXXXXXX "   // Duplicate name, should be erased
      "kernel.ram.reserve.t8=0x5000,0xXXXXXXXXXXXXXXXX "   // This is the real 't8'
      "kernel.ram.reserve.t9=0x8000,0xXXXXXXXXXXXXXXXX ";  // Denied reservation

  struct ExpectedCallback {
    size_t size;
    const char* name;
    std::optional<uintptr_t> to_return;
    bool visited = false;
  };

  std::array expected = {
      ExpectedCallback{0x1000, "t7", 0x5000},
      ExpectedCallback{0x5000, "t8", 0xA000},
      ExpectedCallback{0x8000, "t9", std::nullopt},
  };

  // Populate the command line instance with our test vector.
  c->Append(kTestVector);

  // Now process our data an make sure we get the callbacks we expect.
  c->ProcessRamReservations(
      [&expected](size_t size, std::string_view name) -> std::optional<uintptr_t> {
        // Find our expected results
        auto e = std::find_if(expected.begin(), expected.end(), [&name](const ExpectedCallback& e) {
          return !strncmp(e.name, name.data(), name.size());
        });

        // If we don't find any, then this callback should not have been made.
        if (e == expected.end()) {
          EXPECT_TRUE(false);
          return std::nullopt;
        }

        // Check to make sure the size we received in the callback
        // matches what we expect.
        EXPECT_EQ(e->size, size);

        // Make sure we have not already visited this expected result, then mark it
        // as visited.
        EXPECT_FALSE(e->visited);
        e->visited = true;

        // Finally, return the value the test wants us to.
        return e->to_return;
      });

  // Make sure that we visited all of the nodes we expected to visit
  for (const auto& e : expected) {
    EXPECT_TRUE(e.visited, "callback not made for \"%s\"", e.name);
  }

  // Finally, make sure that the transformed data in the command line buffer matches what we expect.
  static constexpr const char kExpectedBuffer[] =
      "kernel.Ram.reserve.t0=0x2000,0xXXXXXXXXXXXXXXXX\0"   // Wrong argument name
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"    // Missing ','
      "xxxxxxxxxxxxxxxxxxxxxx\0"                            // Missing args
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"   // Bad size
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"   // Bad dynamic allocation placeholder
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"    // Short dynamic allocation placeholder
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"  // Long dynamic allocation placeholder
      "kernel.ram.reserve.t7=0x1000,0x0000000000005000\0"   // Good dynamic allocation
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"   // Duplicate name, should be erased
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0"   // Duplicate name, should be erased
      "kernel.ram.reserve.t8=0x5000,0x000000000000a000\0"   // This is the real 't8'
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\0";  // Denied reservation

  ASSERT_EQ(sizeof(kExpectedBuffer), c->size());
  EXPECT_BYTES_EQ(kExpectedBuffer, c->data(), c->size());
}

}  // namespace
