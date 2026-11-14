// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory_coordinator/memory_consumer_android.h"

// memory_consumer_android.cc forwards these two calls to the Java class
// MemoryConsumerRegistration, and only in browser and WebView processes. In
// every other process they return at once. There is no Java side here, so
// they do nothing.

namespace base::android {

void FlushPendingMemoryConsumerRegistrations() {}

void OnMemoryConsumerRegistryDestroyed() {}

}  // namespace base::android
