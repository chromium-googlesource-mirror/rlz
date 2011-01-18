// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by an Apache-style license that can be
// found in the COPYING file.
//
// A library to manage RLZ information for access-points shared
// across different client applications.

#include "rlz/win/lib/rlz_lib.h"

#include <windows.h>
#include <aclapi.h>
#include <winbase.h>
#include <winerror.h>
#include <vector>

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/win/registry.h"
#include "rlz/win/lib/assert.h"
#include "rlz/win/lib/crc32.h"
#include "rlz/win/lib/financial_ping.h"
#include "rlz/win/lib/lib_mutex.h"
#include "rlz/win/lib/lib_values.h"
#include "rlz/win/lib/machine_deal.h"
#include "rlz/win/lib/string_utils.h"
#include "rlz/win/lib/user_key.h"

namespace {

// Event information returned from ping response.
struct ReturnedEvent {
  rlz_lib::AccessPoint access_point;
  rlz_lib::Event event_type;
};

// Helper functions

bool IsAccessPointSupported(rlz_lib::AccessPoint point, HKEY user_key) {
  switch (point) {
  case rlz_lib::NO_ACCESS_POINT:
  case rlz_lib::LAST_ACCESS_POINT:

  case rlz_lib::MOBILE_IDLE_SCREEN_BLACKBERRY:
  case rlz_lib::MOBILE_IDLE_SCREEN_WINMOB:
  case rlz_lib::MOBILE_IDLE_SCREEN_SYMBIAN:
    // These AP's are never available on Windows PCs.
    return false;

  case rlz_lib::IE_DEFAULT_SEARCH:
  case rlz_lib::IE_HOME_PAGE:
  case rlz_lib::IETB_SEARCH_BOX:
  case rlz_lib::QUICK_SEARCH_BOX:
  case rlz_lib::GD_DESKBAND:
  case rlz_lib::GD_SEARCH_GADGET:
  case rlz_lib::GD_WEB_SERVER:
  case rlz_lib::GD_OUTLOOK:
  case rlz_lib::CHROME_OMNIBOX:
  case rlz_lib::CHROME_HOME_PAGE:
    // TODO: Figure out when these settings are set to Google.

  default:
    return true;
  }
}

// Deletes a registry key if it exists and has no subkeys or values.
// TODO: Move this to a registry_utils file and add unittest.
bool DeleteKeyIfEmpty(HKEY root_key, const wchar_t* key_name) {
  if (!key_name) {
    ASSERT_STRING("DeleteKeyIfEmpty: key_name is NULL");
    return false;
  } else {  // Scope needed for RegKey
    base::win::RegKey key(root_key, key_name, KEY_READ);
    if (!key.Valid())
      return true;  // Key does not exist - nothing to do.

    base::win::RegistryKeyIterator key_iter(root_key, key_name);
    if (key_iter.SubkeyCount() > 0)
      return true;  // Not empty, so nothing to do

    base::win::RegistryValueIterator value_iter(root_key, key_name);
    if (value_iter.ValueCount() > 0)
      return true;  // Not empty, so nothing to do
  }

  // The key is empty - delete it now.
  base::win::RegKey key(root_key, L"", KEY_WRITE);
  return key.DeleteKey(key_name) == ERROR_SUCCESS;
}

// Current RLZ can only uses [a-zA-Z0-9_\-]
// We will be more liberal and allow some additional chars, but not url meta
// chars.
bool IsGoodRlzChar(const char ch) {
  if (IsAsciiAlpha(ch) || IsAsciiDigit(ch))
    return true;

  switch (ch) {
    case '_':
    case '-':
    case '!':
    case '@':
    case '$':
    case '*':
    case '(':
    case ')':
    case ';':
    case '.':
    case '<':
    case '>':
    return true;
  }

  return false;
}

bool IsGoodRlz(const char* rlz) {
  if (!rlz)
    return false;

  if (strlen(rlz) > rlz_lib::kMaxRlzLength)
    return false;

  for (int i = 0; rlz[i]; i++)
    if (!IsGoodRlzChar(rlz[i]))
      return false;

  return true;
}

// This function will remove bad rlz chars and also limit the max rlz to some
// reasonable size.  It also assumes that normalized_rlz is at least
// kMaxRlzLength+1 long.
void NormalizeRlz(const char* raw_rlz, char* normalized_rlz) {
  int index = 0;
  for (; raw_rlz[index] != 0 && index < rlz_lib::kMaxRlzLength; ++index) {
    char current = raw_rlz[index];
    if (IsGoodRlzChar(current)) {
      normalized_rlz[index] = current;
    } else {
      normalized_rlz[index] = '.';
    }
  }

  normalized_rlz[index] = 0;
}

void GetEventsFromResponseString(
    const std::string& response_line,
    const std::string& field_header,
    std::vector<ReturnedEvent>* event_array) {
  // Get the string of events.
  std::string events = response_line.substr(field_header.size());
  TrimWhitespaceASCII(events, TRIM_LEADING, &events);

  int events_length = events.find_first_of("\r\n ");
  if (events_length < 0)
    events_length = events.size();
  events = events.substr(0, events_length);

  // Break this up into individual events
  int event_end_index = -1;
  do {
    int event_begin = event_end_index + 1;
    event_end_index = events.find(rlz_lib::kEventsCgiSeparator, event_begin);
    int event_end = event_end_index;
    if (event_end < 0)
      event_end = events_length;

    std::string event_string = events.substr(event_begin,
                                             event_end - event_begin);
    if (event_string.size() != 3)  // 3 = 2(AP) + 1(E)
      continue;

    rlz_lib::AccessPoint point = rlz_lib::NO_ACCESS_POINT;
    rlz_lib::Event event = rlz_lib::INVALID_EVENT;
    if (!GetAccessPointFromName(event_string.substr(0, 2).c_str(), &point) ||
        point == rlz_lib::NO_ACCESS_POINT) {
      continue;
    }

    if (!GetEventFromName(event_string.substr(event_string.size() - 1).c_str(),
                          &event) || event == rlz_lib::INVALID_EVENT) {
      continue;
    }

    ReturnedEvent current_event = {point, event};
    event_array->push_back(current_event);
  } while (event_end_index >= 0);
}

// Event storage functions.
bool RecordStatefulEvent(rlz_lib::Product product, rlz_lib::AccessPoint point,
                         rlz_lib::Event event, const wchar_t* sid) {
  rlz_lib::LibMutex lock;
  if (lock.failed())
    return false;

  rlz_lib::UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  const wchar_t* product_name = rlz_lib::GetProductName(product);
  if (!product_name)
    return false;

  std::wstring key_name;
  base::StringAppendF(&key_name, L"%ls\\%ls\\%ls", rlz_lib::kLibKeyName,
                      rlz_lib::kStatefulEventsSubkeyName, product_name);

  // Write the new event to registry.
  const char* point_name = GetAccessPointName(point);
  const char* event_name = GetEventName(event);
  if (!point_name || !event_name)
    return false;

  if (!point_name[0] || !event_name[0])
    return false;

  std::wstring point_name_wide(ASCIIToWide(point_name));
  std::wstring event_name_wide(ASCIIToWide(event_name));
  std::wstring new_event_value;
  base::StringAppendF(&new_event_value, L"%ls%ls", point_name_wide.c_str(),
                      event_name_wide.c_str());
  DWORD data = 1;

  base::win::RegKey key(user_key.Get(), key_name.c_str(), KEY_WRITE);
  if (key.WriteValue(new_event_value.c_str(), data) != ERROR_SUCCESS) {
    ASSERT_STRING(
        "RecordStatefulEvent: Could not write the new stateful event");
    return false;
  }

  return true;
}

LONG GetProductEventsAsCgiHelper(rlz_lib::Product product, char* cgi,
                                 size_t cgi_size, HKEY user_key) {
  // Prepend the CGI param key to the buffer.
  std::string cgi_arg;
  base::StringAppendF(&cgi_arg, "%s=", rlz_lib::kEventsCgiVariable);
  if (cgi_size <= cgi_arg.size())
    return ERROR_MORE_DATA;

  size_t index;
  for (index = 0; index < cgi_arg.size(); ++index)
    cgi[index] = cgi_arg[index];

  // Open the events key.
  const wchar_t* product_name = rlz_lib::GetProductName(product);
  if (!product_name)
    return false;

  std::wstring key_name;
  base::StringAppendF(&key_name, L"%ls\\%ls\\%ls", rlz_lib::kLibKeyName,
                      rlz_lib::kEventsSubkeyName, product_name);
  base::win::RegKey events(user_key, key_name.c_str(), KEY_READ);
  if (!events.Valid())
    return ERROR_PATH_NOT_FOUND;

  // Append the events to the buffer.
  size_t buffer_size = cgi_size - cgi_arg.size();
  int num_values = 0;
  LONG result = ERROR_SUCCESS;

  for (num_values = 0; result == ERROR_SUCCESS; ++num_values) {
    cgi[index] = '\0';

    int divider = num_values > 0 ? 1 : 0;
    DWORD size = cgi_size - (index + divider);
    if (size <= 0)
      return ERROR_MORE_DATA;

    result = RegEnumValueA(events.Handle(), num_values, cgi + index + divider,
                           &size, NULL, NULL, NULL, NULL);
    if (result == ERROR_SUCCESS) {
      if (divider)
        cgi[index] = rlz_lib::kEventsCgiSeparator;

      index += size + divider;
    }
  }

  num_values--;
  cgi[index] = '\0';

  if (result == ERROR_MORE_DATA)
    return result;

  return (result == ERROR_NO_MORE_ITEMS && num_values > 0) ?
    ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

bool ClearAllProductEventValues(rlz_lib::Product product, const wchar_t* key,
                                const wchar_t* sid) {
  rlz_lib::LibMutex lock;
  if (lock.failed())
    return false;

  rlz_lib::UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  std::wstring key_name;
  base::StringAppendF(&key_name, L"%ls\\%ls", rlz_lib::kLibKeyName, key);

  const wchar_t* product_name = rlz_lib::GetProductName(product);
  if (!product_name)
    return false;

  base::win::RegKey reg_key(user_key.Get(), key_name.c_str(), KEY_WRITE);
  reg_key.DeleteKey(product_name);

  // Verify that the value no longer exists.
  base::StringAppendF(&key_name, L"\\%ls", product_name);
  base::win::RegKey product_events(user_key.Get(), key_name.c_str(), KEY_READ);
  if (product_events.Valid()) {
    ASSERT_STRING("ClearAllProductEvents: Key deletion failed");
    return false;
  }

  return true;
}

}  // namespace anonymous


namespace rlz_lib {

bool RecordProductEvent(Product product, AccessPoint point, Event event,
                        const wchar_t* sid) {
  LibMutex lock;
  UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  const wchar_t* product_name = GetProductName(product);
  if (!product_name)
    return false;

  // Get this event's value.
  const char* point_name = GetAccessPointName(point);
  const char* event_name = GetEventName(event);
  if (!point_name || !event_name)
    return false;

  if (!point_name[0] || !event_name[0])
    return false;

  std::wstring point_name_wide(ASCIIToWide(point_name));
  std::wstring event_name_wide(ASCIIToWide(event_name));
  std::wstring new_event_value;
  base::StringAppendF(&new_event_value, L"%ls%ls", point_name_wide.c_str(),
                      event_name_wide.c_str());

  // Check whether this event is a stateful event. If so, don't record it.
  std::wstring stateful_key_name;
  base::StringAppendF(&stateful_key_name, L"%ls\\%ls\\%ls", kLibKeyName,
                      kStatefulEventsSubkeyName, product_name);

  DWORD value;
  base::win::RegKey key(user_key.Get(), stateful_key_name.c_str(), KEY_READ);
  if (key.ReadValueDW(new_event_value.c_str(), &value) == ERROR_SUCCESS) {
    // For a stateful event we skip recording, this function is also
    // considered successful.
    return true;
  }

  std::wstring key_name;
  base::StringAppendF(&key_name, L"%ls\\%ls\\%ls", kLibKeyName,
                      kEventsSubkeyName, product_name);

  // Write the new event to registry.
  value = 1;
  base::win::RegKey reg_key(user_key.Get(), key_name.c_str(), KEY_WRITE);
  if (reg_key.WriteValue(new_event_value.c_str(), value) != ERROR_SUCCESS) {
    ASSERT_STRING("RecordProductEvent: Could not write the new event value");
    return false;
  }

  return true;
}

bool ClearProductEvent(Product product, AccessPoint point, Event event,
                       const wchar_t* sid) {
  LibMutex lock;
  if (lock.failed())
    return false;

  UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  const wchar_t* product_name = GetProductName(product);
  if (!product_name)
    return false;

  std::wstring key_name;
  base::StringAppendF(&key_name, L"%ls\\%ls\\%ls", kLibKeyName,
                      kEventsSubkeyName, product_name);

  // Get the event's registry value and delete it.
  const char* point_name = GetAccessPointName(point);
  const char* event_name = GetEventName(event);
  if (!point_name || !event_name)
    return false;

  if (!point_name[0] || !event_name[0])
    return false;

  std::wstring point_name_wide(ASCIIToWide(point_name));
  std::wstring event_name_wide(ASCIIToWide(event_name));
  std::wstring event_value;
  base::StringAppendF(&event_value, L"%ls%ls", point_name_wide.c_str(),
                      event_name_wide.c_str());
  base::win::RegKey key(user_key.Get(), key_name.c_str(), KEY_WRITE);
  key.DeleteValue(event_value.c_str());

  // Verify deletion.
  DWORD value;
  if (key.ReadValueDW(event_value.c_str(), &value) == ERROR_SUCCESS) {
    ASSERT_STRING("ClearProductEvent: Could not delete the event value.");
    return false;
  }

  return true;
}

bool GetProductEventsAsCgi(Product product, char* cgi, size_t cgi_size,
                           const wchar_t* sid) {
  if (!cgi || cgi_size <= 0) {
    ASSERT_STRING("GetProductEventsAsCgi: Invalid buffer");
    return false;
  }

  cgi[0] = 0;

  LibMutex lock;
  if (lock.failed())
    return false;

  UserKey user_key(sid);
  if (!user_key.HasAccess(false))
    return false;

  DWORD size_local =
      cgi_size <= kMaxCgiLength + 1 ? cgi_size : kMaxCgiLength + 1;
  UINT length = 0;
  LONG result = GetProductEventsAsCgiHelper(product, cgi, size_local,
                                            user_key.Get());
  if (result == ERROR_MORE_DATA && cgi_size >= (kMaxCgiLength + 1))
    result = ERROR_SUCCESS;

  if (result != ERROR_SUCCESS) {
    if (result == ERROR_MORE_DATA)
      ASSERT_STRING("GetProductEventsAsCgi: Insufficient buffer size");
    cgi[0] = 0;
    return false;
  }

  return true;
}

bool ClearAllProductEvents(Product product, const wchar_t* sid) {
  bool result;

  result = ClearAllProductEventValues(product, kEventsSubkeyName, sid);
  result &= ClearAllProductEventValues(product, kStatefulEventsSubkeyName, sid);
  return result;
}

// RLZ storage functions.

bool GetAccessPointRlz(AccessPoint point, char* rlz, size_t rlz_size,
                       HKEY user_key) {
  if (!rlz || rlz_size <= 0) {
    ASSERT_STRING("GetAccessPointRlz: Invalid buffer");
    return false;
  }

  rlz[0] = 0;

  LibMutex lock;
  if (lock.failed())
    return false;

  if (!UserKey::HasAccess(user_key, false))
    return false;

  // Return false if the access point is not supported.
  if (!IsAccessPointSupported(point, user_key))
    return false;

  // Open the RLZs key.
  std::wstring rlzs_key_name;
  base::StringAppendF(&rlzs_key_name, L"%ls\\%ls", kLibKeyName,
                      kRlzsSubkeyName);

  // Get the RLZ value.
  const char* access_point_name = GetAccessPointName(point);
  if (!access_point_name)
    return false;

  size_t size = rlz_size;
  base::win::RegKey key(user_key, rlzs_key_name.c_str(), KEY_READ);
  if (!RegKeyReadValue(key, ASCIIToWide(access_point_name).c_str(),
                       rlz, &size)) {
    rlz[0] = 0;
    if (size > rlz_size) {
      ASSERT_STRING("GetAccessPointRlz: Insufficient buffer size");
      return false;
    }
  }

  return true;
}

bool GetAccessPointRlz(AccessPoint point, char* rlz, size_t rlz_size,
                       const wchar_t* sid) {
  UserKey user_key(sid);
  return GetAccessPointRlz(point, rlz, rlz_size, user_key.Get());
}

bool SetAccessPointRlz(AccessPoint point, const char* new_rlz,
                       const wchar_t* sid) {
  LibMutex lock;
  if (lock.failed())
    return false;

  UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  if (!new_rlz) {
    ASSERT_STRING("SetAccessPointRlz: Invalid buffer");
    return false;
  }

  // Return false if the access point is not set to Google.
  if (!IsAccessPointSupported(point, user_key.Get())) {
    ASSERT_STRING(("SetAccessPointRlz: "
                "Cannot set RLZ for unsupported access point."));
    return false;
  }

  // Verify the RLZ length.
  size_t rlz_length = strlen(new_rlz);
  if (rlz_length > kMaxRlzLength) {
    ASSERT_STRING("SetAccessPointRlz: RLZ length is exceeds max allowed.");
    return false;
  }

  char normalized_rlz[kMaxRlzLength + 1];
  NormalizeRlz(new_rlz, normalized_rlz);
  VERIFY(strlen(new_rlz) == rlz_length);

  // Open or create the RLZs key.
  std::wstring rlzs_key_name;
  base::StringAppendF(&rlzs_key_name, L"%ls\\%ls", kLibKeyName,
                      kRlzsSubkeyName);

  // Write the RLZ for this access point.
  const char* access_point_name = GetAccessPointName(point);
  if (!access_point_name)
    return false;

  std::wstring access_point_name_wide(ASCIIToWide(access_point_name));
  base::win::RegKey key(user_key.Get(), rlzs_key_name.c_str(), KEY_WRITE);

  if (normalized_rlz[0] == 0) {
    // Setting RLZ to empty == clearing. Delete the registry value.
    key.DeleteValue(access_point_name_wide.c_str());

    // Verify deletion.
    DWORD value;
    if (key.ReadValueDW(access_point_name_wide.c_str(), &value) ==
        ERROR_SUCCESS) {
      ASSERT_STRING("SetAccessPointRlz: Could not clear the RLZ value.");
      return false;
    }
  } else {
    if (!RegKeyWriteValue(key, access_point_name_wide.c_str(),
                          normalized_rlz)) {
      ASSERT_STRING("SetAccessPointRlz: Could not write the new RLZ value");
      return false;
    }
  }

  return true;
}


// OEM Deal confirmation storage functions.

template<class T>
class typed_buffer_ptr {
  scoped_array<char> buffer_;

 public:
  typed_buffer_ptr() {
  }

  explicit typed_buffer_ptr(size_t size) : buffer_(new char[size]) {
  }

  void reset(size_t size) {
    buffer_.reset(new char[size]);
  }

  operator T*() {
    return reinterpret_cast<T*>(buffer_.get());
  }
};

// Check if this SID has the desired access by scanning the ACEs in the DACL.
// This function is part of the rlz_lib namespace so that it can be called from
// unit tests.  Non-unit test code should not call this function.
bool HasAccess(PSID sid, ACCESS_MASK access_mask, ACL* dacl) {
  if (dacl == NULL)
    return false;

  ACL_SIZE_INFORMATION info;
  if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
    return false;

  GENERIC_MAPPING generic_mapping = {KEY_READ, KEY_WRITE, KEY_EXECUTE,
                                     KEY_ALL_ACCESS};
  MapGenericMask(&access_mask, &generic_mapping);

  for (DWORD i = 0; i < info.AceCount; ++i) {
    ACCESS_ALLOWED_ACE* ace;
    if (GetAce(dacl, i, reinterpret_cast<void**>(&ace))) {
      if ((ace->Header.AceFlags & INHERIT_ONLY_ACE) == INHERIT_ONLY_ACE)
        continue;

      PSID existing_sid = reinterpret_cast<PSID>(&ace->SidStart);
      DWORD mask = ace->Mask;
      MapGenericMask(&mask, &generic_mapping);

      if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
         (mask & access_mask) == access_mask && EqualSid(existing_sid, sid))
        return true;

      if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE &&
         (mask & access_mask) != 0 && EqualSid(existing_sid, sid))
        return false;
    }
  }

  return false;
}

bool CreateMachineState() {
  LibMutex lock;
  if (lock.failed())
    return false;

  base::win::RegKey hklm_key;
  if (hklm_key.Create(HKEY_LOCAL_MACHINE, kLibKeyName,
                      KEY_ALL_ACCESS | KEY_WOW64_32KEY) != ERROR_SUCCESS) {
    ASSERT_STRING("rlz_lib::CreateMachineState: "
                  "Unable to create / open machine key.");
    return false;
  }

  // Create a SID that represents ALL USERS.
  DWORD users_sid_size = SECURITY_MAX_SID_SIZE;
  typed_buffer_ptr<SID> users_sid(users_sid_size);
  CreateWellKnownSid(WinBuiltinUsersSid, NULL, users_sid, &users_sid_size);

  // Get the security descriptor for the registry key.
  DWORD original_sd_size = 0;
  ::RegGetKeySecurity(hklm_key.Handle(), DACL_SECURITY_INFORMATION, NULL,
      &original_sd_size);
  typed_buffer_ptr<SECURITY_DESCRIPTOR> original_sd(original_sd_size);

  LONG result = ::RegGetKeySecurity(hklm_key.Handle(),
      DACL_SECURITY_INFORMATION, original_sd, &original_sd_size);
  if (result != ERROR_SUCCESS) {
    ASSERT_STRING("rlz_lib::CreateMachineState: "
                  "Unable to create / open machine key.");
    return false;
  }

  // Make a copy of the security descriptor so we can modify it.  The one
  // returned by RegGetKeySecurity() is self-relative, so we need to make it
  // absolute.
  DWORD new_sd_size = 0;
  DWORD dacl_size = 0;
  DWORD sacl_size = 0;
  DWORD owner_size = 0;
  DWORD group_size = 0;
  ::MakeAbsoluteSD(original_sd, NULL, &new_sd_size, NULL, &dacl_size,
                        NULL, &sacl_size, NULL, &owner_size,
                        NULL, &group_size);

  typed_buffer_ptr<SECURITY_DESCRIPTOR> new_sd(new_sd_size);
  // Make sure the DACL is big enough to add one more ACE.
  typed_buffer_ptr<ACL> dacl(dacl_size + SECURITY_MAX_SID_SIZE);
  typed_buffer_ptr<ACL> sacl(sacl_size);
  typed_buffer_ptr<SID> owner(owner_size);
  typed_buffer_ptr<SID> group(group_size);

  if (!::MakeAbsoluteSD(original_sd, new_sd, &new_sd_size, dacl, &dacl_size,
                        sacl, &sacl_size, owner, &owner_size,
                        group, &group_size)) {
    ASSERT_STRING("rlz_lib::CreateMachineState: MakeAbsoluteSD failed");
    return false;
  }

  // If all users already have read/write access to the registry key, then
  // nothing to do.  Otherwise change the security descriptor of the key to
  // give everyone access.
  if (HasAccess(users_sid, KEY_ALL_ACCESS, dacl)) {
    return false;
  }

  // Add ALL-USERS ALL-ACCESS ACL.
  EXPLICIT_ACCESS ea;
  ZeroMemory(&ea, sizeof(EXPLICIT_ACCESS));
  ea.grfAccessPermissions = GENERIC_ALL | KEY_ALL_ACCESS;
  ea.grfAccessMode = GRANT_ACCESS;
  ea.grfInheritance= SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
  ea.Trustee.ptstrName = L"Everyone";

  ACL* new_dacl = NULL;
  result = SetEntriesInAcl(1, &ea, dacl, &new_dacl);
  if (result != ERROR_SUCCESS) {
    ASSERT_STRING("rlz_lib::CreateMachineState: SetEntriesInAcl failed");
    return false;
  }

  BOOL ok = SetSecurityDescriptorDacl(new_sd, TRUE, new_dacl, FALSE);
  if (!ok) {
    ASSERT_STRING("rlz_lib::CreateMachineState: "
                  "SetSecurityDescriptorOwner failed");
    LocalFree(new_dacl);
    return false;
  }

  result = ::RegSetKeySecurity(hklm_key.Handle(),
                               DACL_SECURITY_INFORMATION,
                               new_sd);
  // Note that the new DACL cannot be freed until after the call to
  // RegSetKeySecurity().
  LocalFree(new_dacl);

  bool success = true;
  if (result != ERROR_SUCCESS) {
    ASSERT_STRING("rlz_lib::CreateMachineState: "
                  "Unable to create / open machine key.");
    success = false;
  }


  return success;
}

bool SetMachineDealCode(const char* dcc) {
  return MachineDealCode::Set(dcc);
}

bool GetMachineDealCodeAsCgi(char* cgi, size_t cgi_size) {
  return MachineDealCode::GetAsCgi(cgi, cgi_size);
}

bool GetMachineDealCode(char* dcc, size_t dcc_size) {
  return MachineDealCode::Get(dcc, dcc_size);
}

// Combined functions.

bool GetPingParams(Product product, const AccessPoint* access_points,
                   char* cgi, size_t cgi_size, const wchar_t* sid) {
  if (!cgi || cgi_size <= 0) {
    ASSERT_STRING("GetPingParams: Invalid buffer");
    return false;
  }

  cgi[0] = 0;

  LibMutex lock;
  if (lock.failed())
    return false;

  UserKey user_key(sid);
  if (!user_key.HasAccess(false))
    return false;

  if (!access_points) {
    ASSERT_STRING("GetPingParams: access_points is NULL");
    return false;
  }

  // Add the RLZ Exchange Protocol version.
  std::string cgi_string(kProtocolCgiArgument);

  // Copy the &rlz= over.
  base::StringAppendF(&cgi_string, "&%s=", kRlzCgiVariable);

  // Now add each of the RLZ's.
  bool first_rlz = true;  // comma before every RLZ but the first.
  for (int i = 0; access_points[i] != NO_ACCESS_POINT; i++) {
    char rlz[kMaxRlzLength + 1];
    if (GetAccessPointRlz(access_points[i], rlz, arraysize(rlz), sid)) {
      const char* access_point = GetAccessPointName(access_points[i]);
      if (!access_point)
        continue;

      base::StringAppendF(&cgi_string, "%s%s%s%s",
                          first_rlz ? "" : kRlzCgiSeparator,
                          access_point, kRlzCgiIndicator, rlz);
      first_rlz = false;
    }
  }

  // Report the DCC too if not empty.
  char dcc[kMaxDccLength + 1];
  dcc[0] = 0;
  if (GetMachineDealCode(dcc, arraysize(dcc)) && dcc[0])
    base::StringAppendF(&cgi_string, "&%s=%s", kDccCgiVariable, dcc);

  if (cgi_string.size() >= cgi_size)
    return false;

  strncpy(cgi, cgi_string.c_str(), cgi_size);
  cgi[cgi_size - 1] = 0;

  return true;
}

bool IsPingResponseValid(const char* response, int* checksum_idx) {
  if (!response || !response[0])
    return false;

  if (checksum_idx)
    *checksum_idx = -1;

  if (strlen(response) > kMaxPingResponseLength) {
    ASSERT_STRING("IsPingResponseValid: response is too long to parse.");
    return false;
  }

  // Find the checksum line.
  std::string response_string(response);

  std::string checksum_param("\ncrc32: ");
  int calculated_crc;
  int checksum_index = response_string.find(checksum_param);
  if (checksum_index >= 0) {
    // Calculate checksum of message preceeding checksum line.
    // (+ 1 to include the \n)
    std::string message(response_string.substr(0, checksum_index + 1));
    if (!Crc32(message.c_str(), &calculated_crc))
      return false;
  } else {
    checksum_param = "crc32: ";  // Empty response case.
    if (!StartsWithASCII(response_string, checksum_param, true))
      return false;

    checksum_index = 0;
    if (!Crc32("", &calculated_crc))
      return false;
  }

  // Find the checksum value on the response.
  int checksum_end = response_string.find("\n", checksum_index + 1);
  if (checksum_end < 0)
    checksum_end = response_string.size();

  int checksum_begin = checksum_index + checksum_param.size();
  std::string checksum = response_string.substr(checksum_begin,
      checksum_end - checksum_begin + 1);
  TrimWhitespaceASCII(checksum, TRIM_ALL, &checksum);

  if (checksum_idx)
    *checksum_idx = checksum_index;

  return calculated_crc == HexStringToInteger(checksum.c_str());
}

// TODO: Use something like RSA to make sure the response is
// from a Google server.
bool ParsePingResponse(Product product, const char* response,
                       const wchar_t* sid) {
  LibMutex lock;
  if (lock.failed())
    return false;

  UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return false;

  std::string response_string(response);
  int response_length = -1;
  if (!IsPingResponseValid(response, &response_length))
    return false;

  if (0 == response_length)
    return true;  // Empty response - no parsing.

  std::string events_variable;
  std::string stateful_events_variable;
  base::SStringPrintf(&events_variable, "%s: ", kEventsCgiVariable);
  base::SStringPrintf(&stateful_events_variable, "%s: ",
                      kStatefulEventsCgiVariable);

  int rlz_cgi_length = strlen(kRlzCgiVariable);

  // Split response lines. Expected response format is lines of the form:
  // rlzW1: 1R1_____en__252
  int line_end_index = -1;
  do {
    int line_begin = line_end_index + 1;
    line_end_index = response_string.find("\n", line_begin);

    int line_end = line_end_index;
    if (line_end < 0)
      line_end = response_length;

    if (line_end <= line_begin)
      continue;  // Empty line.

    std::string response_line;
    response_line = response_string.substr(line_begin, line_end - line_begin);

    if (StartsWithASCII(response_line, kRlzCgiVariable, true)) {  // An RLZ.
      int separator_index = -1;
      if ((separator_index = response_line.find(": ")) < 0)
        continue;  // Not a valid key-value pair.

      // Get the access point.
      std::string point_name =
        response_line.substr(3, separator_index - rlz_cgi_length);
      AccessPoint point = NO_ACCESS_POINT;
      if (!GetAccessPointFromName(point_name.c_str(), &point) ||
          point == NO_ACCESS_POINT)
        continue;  // Not a valid access point.

      // Get the new RLZ.
      std::string rlz_value(response_line.substr(separator_index + 2));
      TrimWhitespaceASCII(rlz_value, TRIM_LEADING, &rlz_value);

      int rlz_length = rlz_value.find_first_of("\r\n ");
      if (rlz_length < 0)
        rlz_length = rlz_value.size();

      if (rlz_length > kMaxRlzLength)
        continue;  // Too long.

      if (IsAccessPointSupported(point, user_key.Get()))
        SetAccessPointRlz(point, rlz_value.substr(0, rlz_length).c_str(), sid);
    } else if (StartsWithASCII(response_line, events_variable, true)) {
      // Clear events which server parsed.
      std::vector<ReturnedEvent> event_array;
      GetEventsFromResponseString(response_line, events_variable, &event_array);
      for (size_t i = 0; i < event_array.size(); ++i) {
        ClearProductEvent(product, event_array[i].access_point,
                          event_array[i].event_type, sid);
      }
    } else if (StartsWithASCII(response_line, stateful_events_variable, true)) {
      // Record any stateful events the server send over.
      std::vector<ReturnedEvent> event_array;
      GetEventsFromResponseString(response_line, stateful_events_variable,
                                  &event_array);
      for (size_t i = 0; i < event_array.size(); ++i) {
        RecordStatefulEvent(product, event_array[i].access_point,
                            event_array[i].event_type, sid);
      }
    }
  } while (line_end_index >= 0);

  // Update the DCC in registry if needed.
  MachineDealCode::SetFromPingResponse(response);

  return true;
}

bool SetMachineDealCodeFromPingResponse(const char* response) {
  return MachineDealCode::SetFromPingResponse(response);
}

bool FormFinancialPingRequest(Product product, const AccessPoint* access_points,
                              const char* product_signature,
                              const char* product_brand,
                              const char* product_id,
                              const char* product_lang,
                              bool exclude_machine_id,
                              char* request, size_t request_buffer_size,
                              const wchar_t* sid) {
  if (!request || request_buffer_size == 0)
    return false;
  request[0] = 0;

  std::string request_string;
  if (!FinancialPing::FormRequest(product, access_points, product_signature,
                                  product_brand, product_id, product_lang,
                                  exclude_machine_id, sid, &request_string))
    return false;

  if ((request_string.size() < 0) ||
      (static_cast<DWORD>(request_string.size()) >= request_buffer_size))
    return false;

  strncpy(request, request_string.c_str(), request_buffer_size);
  request[request_buffer_size - 1] = 0;
  return true;
}


bool PingFinancialServer(Product product, const char* request, char* response,
                         size_t response_buffer_size, const wchar_t* sid) {
  if (!response || response_buffer_size == 0)
    return false;
  response[0] = 0;

  // Check if the time is right to ping.
  if (!FinancialPing::IsPingTime(product, sid, false)) return false;

  // Send out the ping.
  std::string response_string;
  if (!FinancialPing::PingServer(request, &response_string))
    return false;

  if ((response_string.size() < 0) ||
      (static_cast<DWORD>(response_string.size()) >= response_buffer_size))
    return false;

  strncpy(response, response_string.c_str(), response_buffer_size);
  response[response_buffer_size - 1] = 0;
  return true;
}


bool ParseFinancialPingResponse(Product product, const char* response,
                                const wchar_t* sid) {
  // Update the last ping time irrespective of success.
  FinancialPing::UpdateLastPingTime(product, sid);
  // Parse the ping response - update RLZs, clear events.
  return FinancialPing::ParseResponse(product, response, sid);
}

bool SendFinancialPing(Product product, const AccessPoint* access_points,
                       const char* product_signature,
                       const char* product_brand,
                       const char* product_id, const char* product_lang,
                       bool exclude_machine_id, const wchar_t* sid) {
  return SendFinancialPing(product, access_points, product_signature,
                           product_brand, product_id, product_lang,
                           exclude_machine_id, sid, false);
}


bool SendFinancialPing(Product product, const AccessPoint* access_points,
                       const char* product_signature,
                       const char* product_brand,
                       const char* product_id, const char* product_lang,
                       bool exclude_machine_id, const wchar_t* sid,
                       const bool skip_time_check) {
  // Create the financial ping request.
  std::string request;
  if (!FinancialPing::FormRequest(product, access_points, product_signature,
                                  product_brand, product_id, product_lang,
                                  exclude_machine_id, sid, &request))
    return false;

  // Check if the time is right to ping.
  if (!FinancialPing::IsPingTime(product, sid, skip_time_check))
    return false;

  // Send out the ping, update the last ping time irrespective of success.
  FinancialPing::UpdateLastPingTime(product, sid);
  std::string response;
  if (!FinancialPing::PingServer(request.c_str(), &response))
    return false;

  // Parse the ping response - update RLZs, clear events.
  return FinancialPing::ParseResponse(product, response.c_str(), sid);
}


void ClearProductState(Product product, const AccessPoint* access_points,
                       const wchar_t* sid) {
  LibMutex lock;
  if (lock.failed())
    return;

  UserKey user_key(sid);
  if (!user_key.HasAccess(true))
    return;

  // Delete all product specific state.
  VERIFY(ClearAllProductEvents(product, sid));
  VERIFY(FinancialPing::ClearLastPingTime(product, sid));

  // Delete all RLZ's for access points being uninstalled.
  if (access_points) {
    for (int i = 0; access_points[i] != NO_ACCESS_POINT; i++) {
      VERIFY(SetAccessPointRlz(access_points[i], "" , sid));
    }
  }

  // Delete each of the knows subkeys if empty.
  const wchar_t* subkeys[] = {
    kRlzsSubkeyName,
    kEventsSubkeyName,
    kPingTimesSubkeyName
  };

  for (int i = 0; i < arraysize(subkeys); i++) {
    std::wstring subkey_name;
    base::StringAppendF(&subkey_name, L"%ls\\%ls", kLibKeyName, subkeys[i]);
    VERIFY(DeleteKeyIfEmpty(user_key.Get(), subkey_name.c_str()));
  }

  // Delete the library key and its parents too now if empty.
  VERIFY(DeleteKeyIfEmpty(user_key.Get(), kLibKeyName));
  VERIFY(DeleteKeyIfEmpty(user_key.Get(), kGoogleCommonKeyName));
  VERIFY(DeleteKeyIfEmpty(user_key.Get(), kGoogleKeyName));
}

bool GetMachineId(wchar_t* buffer, size_t buffer_size) {
  if (!buffer || buffer_size <= kMachineIdLength)
    return false;
  buffer[0] = 0;

  std::wstring machine_id;
  if (!MachineDealCode::GetMachineId(&machine_id))
    return false;

  wcsncpy(buffer, machine_id.c_str(), buffer_size);
  buffer[buffer_size - 1] = 0;
  return true;
}

}  // namespace rlz_lib
