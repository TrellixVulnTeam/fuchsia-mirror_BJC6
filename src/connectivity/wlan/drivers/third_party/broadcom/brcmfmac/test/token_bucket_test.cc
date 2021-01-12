// Copyright (c) 2020 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/token_bucket.h"

#include <gtest/gtest.h>

#include <zircon/syscalls.h>

// Set up our own zx_ticks_get for this test so we can control time
static zx_ticks_t g_current_ticks = 0;
__EXPORT zx_ticks_t zx_ticks_get() {
  return g_current_ticks;
}

namespace {

using wlan::brcmfmac::TokenBucket;

TEST(TokenBucket, TicksGetOverride) {
  // Ensure that our zx_ticks_get override works
  g_current_ticks = 0;
  ASSERT_EQ(0, zx_ticks_get());
  g_current_ticks = 3;
  ASSERT_EQ(3, zx_ticks_get());
}

TEST(TokenBucket, ConsumeInitialTokens) {
  TokenBucket bucket(1.0, 1);

  // Ensure there are enough tokens to consume
  ASSERT_TRUE(bucket.consume());
  // And after that there should be no tokens left
  ASSERT_FALSE(bucket.consume());
}

TEST(TokenBucket, ConsumeMultipleTokens) {
  TokenBucket bucket(1.0, 2); // Initial capacity of 2 tokens

  // Consume two tokens right away
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  // Third should not be allowed
  ASSERT_FALSE(bucket.consume());
}

TEST(TokenBucket, TokenGeneration) {
  TokenBucket bucket(1.0);

  // Consume initial token
  ASSERT_TRUE(bucket.consume());
  // Advance one second's worth of ticks
  g_current_ticks += zx_ticks_per_second();
  // Now another token should be available
  ASSERT_TRUE(bucket.consume());
}

TEST(TokenBucket, TokenCapacity) {
  TokenBucket bucket(1.0, 3); // Initial capacity of 3 tokens

  // Consume one token, we should now be left at 2 tokens left in the bucket
  ASSERT_TRUE(bucket.consume());
  // Advance time by 5 seconds, we should now be back at 3 tokens but no more
  g_current_ticks += 5 * zx_ticks_per_second();
  // Consume all three tokens
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  // And further attempts should fail
  ASSERT_FALSE(bucket.consume());
}

TEST(TokenBucket, TokenGenerationRate) {
  TokenBucket bucket(5.0, 3); // 5 tokens per second, 3 initial capacity

  // Consume initial tokens
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  // Advance half a second
  g_current_ticks += zx_ticks_per_second() / 2;
  // Now two tokens should be available
  ASSERT_TRUE(bucket.consume());
  ASSERT_TRUE(bucket.consume());
  // We're only halfway to the third token so no more than that
  ASSERT_FALSE(bucket.consume());
}

TEST(TokenBucket, TokenGenerationRateLessThanOne) {
  TokenBucket bucket(0.5, 1); // Half a token per second, 1 initial capacity

  // Consume initial token
  ASSERT_TRUE(bucket.consume());
  g_current_ticks += zx_ticks_per_second();
  // Advanced one second but token should still not be available
  ASSERT_FALSE(bucket.consume());
  g_current_ticks += zx_ticks_per_second();
  // Now there should be exactly one token available
  ASSERT_TRUE(bucket.consume());
  ASSERT_FALSE(bucket.consume());
}

} // namespace
