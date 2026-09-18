#pragma once

static constexpr const char* third_party_notices = R"UPP_NOTICE(
# Third-party notices

## HIDAPI Windows preparsed-data representation

The Windows raw preparsed-data decoder uses the recovered representation described by HIDAPI as an implementation reference. No HIDAPI DLL is required or loaded. The bounded byte decoder and the existing semantic model are compiled into the executable.

Pinned reference revision: `852cc68b8e0e4c7eba0815ef992951cd8b75c55b`.

Reference files:

- https://github.com/libusb/hidapi/blob/852cc68b8e0e4c7eba0815ef992951cd8b75c55b/windows/hidapi_descriptor_reconstruct.h
- https://github.com/libusb/hidapi/blob/852cc68b8e0e4c7eba0815ef992951cd8b75c55b/windows/hidapi_descriptor_reconstruct.c
- https://github.com/libusb/hidapi/blob/852cc68b8e0e4c7eba0815ef992951cd8b75c55b/LICENSE-bsd.txt

The reference source headers identify:

HIDAPI - Multi-Platform library for communication with HID devices.

libusb/hidapi Team

Copyright 2022, All Rights Reserved.

HIDAPI offers a choice of licenses. The BSD-style license is selected for this reference/adaptation. The original BSD notice is reproduced below and retained verbatim in `LICENSES/HIDAPI-BSD.txt`. `universal_usb_parser.exe --licenses` prints this notice, including the complete license, without accessing any device. Source and binary redistribution must retain the applicable notices.

```text
Copyright (c) 2010, Alan Ott, Signal 11 Software
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Signal 11 Software nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

```
)UPP_NOTICE";
