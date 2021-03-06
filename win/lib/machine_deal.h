// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by an Apache-style license that can be
// found in the COPYING file.
//
// Library functions related to the OEM Deal Confirmation Code.

#ifndef RLZ_WIN_LIB_MACHINE_DEAL_H_
#define RLZ_WIN_LIB_MACHINE_DEAL_H_

#include <string>
#include "rlz/win/lib/rlz_lib.h"

namespace rlz_lib {

class MachineDealCode {
 public:
  // Set the OEM Deal Confirmation Code (DCC). This information is used for RLZ
  // initalization. Must have write access to HKLM - SYSTEM or admin, unless
  // rlz_lib::CreateMachineState() has been successfully called.
  static bool Set(const char* dcc);

  // Get the OEM Deal Confirmation Code from the registry. Used to ping
  // the server.
  static bool Get(AccessPoint point,
                  char* dcc,
                  int dcc_size,
                  const wchar_t* sid = NULL);

  // Parses a ping response, checks if it is valid and sets the machine DCC
  // from the response. The response should also contain the current value of
  // the DCC to be considered valid.
  // Write access to HKLM (system / admin) needed, unless
  // rlz_lib::CreateMachineState() has been successfully called.
  static bool SetFromPingResponse(const char* response);

  // Gets the new DCC to set from a ping response. Returns true if the ping
  // response is valid. Sets has_new_dcc true if there is a new DCC value.
  static bool GetNewCodeFromPingResponse(const char* response,
                                         bool* has_new_dcc,
                                         char* new_dcc,
                                         int new_dcc_size);

  // Get the DCC cgi argument string to append to a daily or financial ping.
  static bool GetAsCgi(char* cgi, int cgi_size);

  // Get the machine code.
  static bool Get(char* dcc, int dcc_size);

  // Gets the universal ID for the machine - this is a hash of the Windows
  // machine SID plus a checksum byte.
  static bool GetMachineId(std::wstring* id);

  // Calculates the universal ID for a machine given an sid and volume id.
  static bool GetMachineIdImpl(const std::wstring& sid_string,
                               int volume_id,
                               std::wstring* id);

 protected:
  // Clear the DCC value. Only for testing purposes.
  // Requires write access to HKLM, unless rlz_lib::CreateMachineState() has
  // been successfully called.
  static bool Clear();

  // Helper for DCC extraction from ping responses.
  // If set_value = true, it extracts the new DCC value to write to registry,
  // if false, it extracts the server's echo of the current DCC value.
  static bool ExtractFromResponse(const char* response,
                                  char* dcc,
                                  int dcc_size,
                                  bool set_value);

  MachineDealCode() {}
  ~MachineDealCode() {}
};

}  // namespace rlz_lib

#endif  // RLZ_WIN_LIB_MACHINE_DEAL_H_
