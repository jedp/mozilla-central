/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jsion_asmjssignalhandlers_h__)
#define jsion_asmjssignalhandlers_h__

namespace js {

// Install any process-level signal/exception-handlers needed to support asm.js.
bool
EnsureAsmJSSignalHandlersInstalled();

// Force any executing asm.js code to call js_HandleExecutionInterrupt.
void
TriggerOperationCallbackForAsmJSCode(JSRuntime *rt);

}  // namespace js

#endif  // jsion_asmjssignalhandlers_h__

