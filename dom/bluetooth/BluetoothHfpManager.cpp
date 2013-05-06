/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

#include "BluetoothHfpManager.h"

#include "BluetoothReplyRunnable.h"
#include "BluetoothScoManager.h"
#include "BluetoothService.h"
#include "BluetoothSocket.h"
#include "BluetoothUtils.h"
#include "BluetoothUuid.h"

#include "MobileConnection.h"
#include "mozilla/dom/bluetooth/BluetoothTypes.h"
#include "mozilla/Hal.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "nsContentUtils.h"
#include "nsIAudioManager.h"
#include "nsIObserverService.h"
#include "nsISettingsService.h"
#include "nsIRadioInterfaceLayer.h"
#include "nsRadioInterfaceLayer.h"

#include <unistd.h> /* usleep() */

#define AUDIO_VOLUME_BT_SCO "audio.volume.bt_sco"
#define MOZSETTINGS_CHANGED_ID "mozsettings-changed"
#define MOBILE_CONNECTION_ICCINFO_CHANGED "mobile-connection-iccinfo-changed"
#define MOBILE_CONNECTION_VOICE_CHANGED "mobile-connection-voice-changed"

/**
 * These constants are used in result code such as +CLIP and +CCWA. The value
 * of these constants is the same as TOA_INTERNATIONAL/TOA_UNKNOWN defined in
 * ril_consts.js
 */
#define TOA_UNKNOWN 0x81
#define TOA_INTERNATIONAL 0x91

#define CR_LF "\xd\xa";

using namespace mozilla;
using namespace mozilla::ipc;
USING_BLUETOOTH_NAMESPACE

namespace {
  StaticAutoPtr<BluetoothHfpManager> gBluetoothHfpManager;
  StaticRefPtr<BluetoothHfpManagerObserver> sHfpObserver;
  bool gInShutdown = false;
  static bool sStopSendingRingFlag = true;
  static int sRingInterval = 3000; //unit: ms
  static const char kHfpCrlf[] = "\xd\xa";
} // anonymous namespace

/* CallState for sCINDItems[CINDType::CALL].value
 * - NO_CALL: there are no calls in progress
 * - IN_PROGRESS: at least one call is in progress
 */
enum CallState {
  NO_CALL,
  IN_PROGRESS
};

/* CallSetupState for sCINDItems[CINDType::CALLSETUP].value
 * - NO_CALLSETUP: not currently in call set up
 * - INCOMING: an incoming call process ongoing
 * - OUTGOING: an outgoing call set up is ongoing
 * - OUTGOING_ALERTING: remote party being alerted in an outgoing call
 */
enum CallSetupState {
  NO_CALLSETUP,
  INCOMING,
  OUTGOING,
  OUTGOING_ALERTING
};

/* CallHeldState for sCINDItems[CINDType::CALLHELD].value
 * - NO_CALLHELD: no calls held
 * - ONHOLD_ACTIVE: both an active and a held call
 * - ONHOLD_NOACTIVE: call on hold, no active call
 */
enum CallHeldState {
  NO_CALLHELD,
  ONHOLD_ACTIVE,
  ONHOLD_NOACTIVE
};

typedef struct {
  const char* name;
  const char* range;
  int value;
} CINDItem;

enum CINDType {
  BATTCHG = 1,
  CALL,
  CALLHELD,
  CALLSETUP,
  SERVICE,
  SIGNAL,
  ROAM
};

static CINDItem sCINDItems[] = {
  {},
  {"battchg", "0-5", 5},
  {"call", "0,1", CallState::NO_CALL},
  {"callheld", "0-2", CallHeldState::NO_CALLHELD},
  {"callsetup", "0-3", CallSetupState::NO_CALLSETUP},
  {"service", "0,1", 0},
  {"signal", "0-5", 0},
  {"roam", "0,1", 0}
};

class mozilla::dom::bluetooth::BluetoothHfpManagerObserver : public nsIObserver
                                                           , public BatteryObserver
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  BluetoothHfpManagerObserver()
  {
  }

  bool Init()
  {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    MOZ_ASSERT(obs);
    if (NS_FAILED(obs->AddObserver(this, MOZSETTINGS_CHANGED_ID, false))) {
      NS_WARNING("Failed to add settings change observer!");
      return false;
    }

    if (NS_FAILED(obs->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false))) {
      NS_WARNING("Failed to add shutdown observer!");
      return false;
    }

    if (NS_FAILED(obs->AddObserver(this, MOBILE_CONNECTION_ICCINFO_CHANGED, false))) {
      NS_WARNING("Failed to add mobile connection iccinfo change observer!");
      return false;
    }

    if (NS_FAILED(obs->AddObserver(this, MOBILE_CONNECTION_VOICE_CHANGED, false))) {
      NS_WARNING("Failed to add mobile connection voice change observer!");
      return false;
    }

    hal::RegisterBatteryObserver(this);

    return true;
  }

  bool Shutdown()
  {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (!obs ||
        NS_FAILED(obs->RemoveObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) ||
        NS_FAILED(obs->RemoveObserver(this, MOZSETTINGS_CHANGED_ID)) ||
        NS_FAILED(obs->RemoveObserver(this, MOBILE_CONNECTION_ICCINFO_CHANGED)) ||
        NS_FAILED(obs->RemoveObserver(this, MOBILE_CONNECTION_VOICE_CHANGED))) {
      NS_WARNING("Can't unregister observers, or already unregistered!");
      return false;
    }

    hal::UnregisterBatteryObserver(this);

    return true;
  }

  ~BluetoothHfpManagerObserver()
  {
    Shutdown();
  }

  void Notify(const hal::BatteryInformation& aBatteryInfo)
  {
    // Range of battery level: [0, 1], double
    // Range of CIND::BATTCHG: [0, 5], int
    int level = ceil(aBatteryInfo.level() * 5.0);
    if (level != sCINDItems[CINDType::BATTCHG].value) {
      sCINDItems[CINDType::BATTCHG].value = level;
      gBluetoothHfpManager->SendCommand("+CIEV: ", CINDType::BATTCHG);
    }
  }
};

class GetVolumeTask : public nsISettingsServiceCallback
{
public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD
  Handle(const nsAString& aName, const jsval& aResult)
  {
    MOZ_ASSERT(NS_IsMainThread());

    JSContext *cx = nsContentUtils::GetCurrentJSContext();
    NS_ENSURE_TRUE(cx, NS_OK);

    if (!aResult.isNumber()) {
      NS_WARNING("'" AUDIO_VOLUME_BT_SCO "' is not a number!");
      return NS_OK;
    }

    BluetoothHfpManager* hfp = BluetoothHfpManager::Get();
    hfp->SetVolume(aResult.toNumber());

    return NS_OK;
  }

  NS_IMETHOD
  HandleError(const nsAString& aName)
  {
    NS_WARNING("Unable to get value for '" AUDIO_VOLUME_BT_SCO "'");
    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS1(GetVolumeTask, nsISettingsServiceCallback);

NS_IMETHODIMP
BluetoothHfpManagerObserver::Observe(nsISupports* aSubject,
                                     const char* aTopic,
                                     const PRUnichar* aData)
{
  MOZ_ASSERT(gBluetoothHfpManager);

  if (!strcmp(aTopic, MOZSETTINGS_CHANGED_ID)) {
    return gBluetoothHfpManager->HandleVolumeChanged(nsDependentString(aData));
  } else if (!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID)) {
    return gBluetoothHfpManager->HandleShutdown();
  } else if (!strcmp(aTopic, MOBILE_CONNECTION_ICCINFO_CHANGED)) {
    return gBluetoothHfpManager->HandleIccInfoChanged();
  } else if (!strcmp(aTopic, MOBILE_CONNECTION_VOICE_CHANGED)) {
    return gBluetoothHfpManager->HandleVoiceConnectionChanged();
  }

  MOZ_ASSERT(false, "BluetoothHfpManager got unexpected topic!");
  return NS_ERROR_UNEXPECTED;
}

NS_IMPL_ISUPPORTS1(BluetoothHfpManagerObserver, nsIObserver)

class SendRingIndicatorTask : public Task
{
public:
  SendRingIndicatorTask(const char* aNumber, int aType = TOA_UNKNOWN)
    : mNumber(aNumber)
    , mType(aType)
  {
    MOZ_ASSERT(NS_IsMainThread());
  }

  void Run() MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());

    NS_ENSURE_FALSE_VOID(sStopSendingRingFlag);

    if (!gBluetoothHfpManager) {
      NS_WARNING("BluetoothHfpManager no longer exists, cannot send ring!");
      return;
    }

    gBluetoothHfpManager->SendLine("RING");

    if (!mNumber.IsEmpty()) {
      nsAutoCString resultCode("+CLIP: \"");
      resultCode += mNumber;
      resultCode += "\",";
      resultCode.AppendInt(mType);

      gBluetoothHfpManager->SendLine(resultCode.get());
    }

    MessageLoop::current()->
      PostDelayedTask(FROM_HERE,
                      new SendRingIndicatorTask(mNumber.get(), mType),
                      sRingInterval);
  }

private:
  nsCString mNumber;
  int mType;
};

void
OpenScoSocket(const nsAString& aDeviceAddress)
{
  MOZ_ASSERT(NS_IsMainThread());

  BluetoothScoManager* sco = BluetoothScoManager::Get();
  if (!sco) {
    NS_WARNING("BluetoothScoManager is not available!");
    return;
  }

  if (!sco->Connect(aDeviceAddress)) {
    NS_WARNING("Failed to create a sco socket!");
  }
}

void
CloseScoSocket()
{
  MOZ_ASSERT(NS_IsMainThread());

  BluetoothScoManager* sco = BluetoothScoManager::Get();
  if (!sco) {
    NS_WARNING("BluetoothScoManager is not available!");
    return;
  }
  sco->Disconnect();
}

BluetoothHfpManager::BluetoothHfpManager()
  : mCurrentCallIndex(0)
  , mReceiveVgsFlag(false)
{
  sCINDItems[CINDType::CALL].value = CallState::NO_CALL;
  sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
  sCINDItems[CINDType::CALLHELD].value = CallHeldState::NO_CALLHELD;

  mCurrentCallStateArray.AppendElement((int)nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED);
}

bool
BluetoothHfpManager::Init()
{
  MOZ_ASSERT(NS_IsMainThread());

  sHfpObserver = new BluetoothHfpManagerObserver();
  if (!sHfpObserver->Init()) {
    NS_WARNING("Cannot set up Hfp Observers!");
  }

  mListener = new BluetoothRilListener();
  if (!mListener->StartListening()) {
    NS_WARNING("Failed to start listening RIL");
    return false;
  }

  nsCOMPtr<nsISettingsService> settings =
    do_GetService("@mozilla.org/settingsService;1");
  NS_ENSURE_TRUE(settings, false);

  nsCOMPtr<nsISettingsServiceLock> settingsLock;
  nsresult rv = settings->CreateLock(getter_AddRefs(settingsLock));
  NS_ENSURE_SUCCESS(rv, false);

  nsRefPtr<GetVolumeTask> callback = new GetVolumeTask();
  rv = settingsLock->Get(AUDIO_VOLUME_BT_SCO, callback);
  NS_ENSURE_SUCCESS(rv, false);

  Listen();

  return true;
}

BluetoothHfpManager::~BluetoothHfpManager()
{
  Cleanup();
}

void
BluetoothHfpManager::Cleanup()
{
  if (!mListener->StopListening()) {
    NS_WARNING("Failed to stop listening RIL");
  }
  mListener = nullptr;

  sHfpObserver->Shutdown();
  sHfpObserver = nullptr;
}

//static
BluetoothHfpManager*
BluetoothHfpManager::Get()
{
  MOZ_ASSERT(NS_IsMainThread());

  // If we already exist, exit early
  if (gBluetoothHfpManager) {
    return gBluetoothHfpManager;
  }

  // If we're in shutdown, don't create a new instance
  if (gInShutdown) {
    NS_WARNING("BluetoothHfpManager can't be created during shutdown");
    return nullptr;
  }

  // Create new instance, register, return
  BluetoothHfpManager* manager = new BluetoothHfpManager();
  NS_ENSURE_TRUE(manager->Init(), nullptr);

  gBluetoothHfpManager = manager;
  return gBluetoothHfpManager;
}

void
BluetoothHfpManager::SetVolume(const int aVolume)
{
  mCurrentVgs = aVolume;
}

void
BluetoothHfpManager::NotifySettings()
{
  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-hfp-status-changed");

  name.AssignLiteral("connected");
  v = IsConnected();
  parameters.AppendElement(BluetoothNamedValue(name, v));

  name.AssignLiteral("address");
  v = mDevicePath;
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to broadcast system message to settings");
    return;
  }
}

void
BluetoothHfpManager::NotifyDialer(const nsAString& aCommand)
{
  nsString type, name;
  BluetoothValue v;
  InfallibleTArray<BluetoothNamedValue> parameters;
  type.AssignLiteral("bluetooth-dialer-command");

  name.AssignLiteral("command");
  v = nsString(aCommand);
  parameters.AppendElement(BluetoothNamedValue(name, v));

  if (!BroadcastSystemMessage(type, parameters)) {
    NS_WARNING("Failed to broadcast system message to dialer");
    return;
  }
}

nsresult
BluetoothHfpManager::HandleVolumeChanged(const nsAString& aData)
{
  MOZ_ASSERT(NS_IsMainThread());

  // The string that we're interested in will be a JSON string that looks like:
  //  {"key":"volumeup", "value":10}
  //  {"key":"volumedown", "value":2}

  JSContext* cx = nsContentUtils::GetSafeJSContext();
  if (!cx) {
    NS_WARNING("Failed to get JSContext");
    return NS_OK;
  }

  JS::Value val;
  if (!JS_ParseJSON(cx, aData.BeginReading(), aData.Length(), &val)) {
    return JS_ReportPendingException(cx) ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
  }

  if (!val.isObject()) {
    return NS_OK;
  }

  JSObject& obj(val.toObject());

  JS::Value key;
  if (!JS_GetProperty(cx, &obj, "key", &key)) {
    MOZ_ASSERT(!JS_IsExceptionPending(cx));
    return NS_ERROR_OUT_OF_MEMORY;
  }

  if (!key.isString()) {
    return NS_OK;
  }

  JSBool match;
  if (!JS_StringEqualsAscii(cx, key.toString(), AUDIO_VOLUME_BT_SCO, &match)) {
    MOZ_ASSERT(!JS_IsExceptionPending(cx));
    return NS_ERROR_OUT_OF_MEMORY;
  }

  if (!match) {
    return NS_OK;
  }

  JS::Value value;
  if (!JS_GetProperty(cx, &obj, "value", &value)) {
    MOZ_ASSERT(!JS_IsExceptionPending(cx));
    return NS_ERROR_OUT_OF_MEMORY;
  }

  if (!value.isNumber()) {
    return NS_ERROR_UNEXPECTED;
  }

  mCurrentVgs = value.toNumber();

  // Adjust volume by headset and we don't have to send volume back to headset
  if (mReceiveVgsFlag) {
    mReceiveVgsFlag = false;
    return NS_OK;
  }

  // Only send volume back when there's a connected headset
  if (IsConnected()) {
    SendCommand("+VGS: ", mCurrentVgs);
  }

  return NS_OK;
}

nsresult
BluetoothHfpManager::HandleVoiceConnectionChanged()
{
  nsCOMPtr<nsIMobileConnectionProvider> connection =
    do_GetService(NS_RILCONTENTHELPER_CONTRACTID);
  NS_ENSURE_TRUE(connection, NS_ERROR_FAILURE);

  nsIDOMMozMobileConnectionInfo* voiceInfo;
  connection->GetVoiceConnectionInfo(&voiceInfo);
  NS_ENSURE_TRUE(voiceInfo, NS_ERROR_FAILURE);

  bool roaming;
  voiceInfo->GetRoaming(&roaming);
  if (roaming != sCINDItems[CINDType::ROAM].value) {
    sCINDItems[CINDType::ROAM].value = roaming;
    SendCommand("+CIEV: ", CINDType::ROAM);
  }

  bool service = false;
  nsString regState;
  voiceInfo->GetState(regState);
  if (regState.EqualsLiteral("registered")) {
    service = true;
  }
  if (service != sCINDItems[CINDType::SERVICE].value) {
    sCINDItems[CINDType::SERVICE].value = service;
    SendCommand("+CIEV: ", CINDType::SERVICE);
  }

  uint8_t signal;
  JS::Value value;
  voiceInfo->GetRelSignalStrength(&value);
  if (!value.isNumber()) {
    NS_WARNING("Failed to get relSignalStrength in BluetoothHfpManager");
    return NS_ERROR_FAILURE;
  }
  signal = ceil(value.toNumber() / 20.0);

  if (signal != sCINDItems[CINDType::SIGNAL].value) {
    sCINDItems[CINDType::SIGNAL].value = signal;
    SendCommand("+CIEV: ", CINDType::SIGNAL);
  }

  return NS_OK;
}

nsresult
BluetoothHfpManager::HandleIccInfoChanged()
{
  nsCOMPtr<nsIMobileConnectionProvider> connection =
    do_GetService(NS_RILCONTENTHELPER_CONTRACTID);
  NS_ENSURE_TRUE(connection, NS_ERROR_FAILURE);

  nsIDOMMozMobileICCInfo* iccInfo;
  connection->GetIccInfo(&iccInfo);
  NS_ENSURE_TRUE(iccInfo, NS_ERROR_FAILURE);

  nsString msisdn;
  iccInfo->GetMsisdn(msisdn);

  if (!msisdn.Equals(mMsisdn)) {
    mMsisdn = msisdn;
  }

  return NS_OK;
}

nsresult
BluetoothHfpManager::HandleShutdown()
{
  MOZ_ASSERT(NS_IsMainThread());
  gInShutdown = true;
  Disconnect();
  gBluetoothHfpManager = nullptr;
  return NS_OK;
}

// Virtual function of class SocketConsumer
void
BluetoothHfpManager::ReceiveSocketData(BluetoothSocket* aSocket,
                                       nsAutoPtr<UnixSocketRawData>& aMessage)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aSocket);

  int currentCallState = mCurrentCallStateArray[mCurrentCallIndex];

  nsAutoCString msg((const char*)aMessage->mData.get(), aMessage->mSize);
  msg.StripWhitespace();

  nsTArray<nsCString> atCommandValues;

  // For more information, please refer to 4.34.1 "Bluetooth Defined AT
  // Capabilities" in Bluetooth hands-free profile 1.6
  if (msg.Find("AT+BRSF=") != -1) {
    SendCommand("+BRSF: ", 33);
  } else if (msg.Find("AT+CIND=?") != -1) {
    // Asking for CIND range
    SendCommand("+CIND: ", 0);
  } else if (msg.Find("AT+CIND?") != -1) {
    // Asking for CIND value
    SendCommand("+CIND: ", 1);
  } else if (msg.Find("AT+CMER=") != -1) {
    /**
     * SLC establishment is done when AT+CMER has been received.
     * Do nothing but respond with "OK".
     */
  } else if (msg.Find("AT+CHLD=?") != -1) {
    SendLine("+CHLD: (1,2)");
  } else if (msg.Find("AT+CHLD=") != -1) {
    ParseAtCommand(msg, 8, atCommandValues);

    if (atCommandValues.IsEmpty()) {
      NS_WARNING("Could't get the value of command [AT+CHLD=]");
      goto respond_with_ok;
    }

    /**
     * The following two cases are supported:
     * AT+CHLD=1 - Releases active calls and accepts the other (held or
     *             waiting) call
     * AT+CHLD=2 - Places active calls on hold and accepts the other (held
     *             or waiting) call
     *
     * The following cases are NOT supported yet:
     * AT+CHLD=0, AT+CHLD=1<idx>, AT+CHLD=2<idx>, AT+CHLD=3, AT+CHLD=4
     * Please see 4.33.2 in Bluetooth hands-free profile 1.6 for more
     * information.
     */
    char chld = atCommandValues[0][0];
    bool valid = true;
    if (atCommandValues[0].Length() > 1) {
      NS_WARNING("No index should be included in command [AT+CHLD]");
      valid = false;
    } else if (chld == '0' || chld == '3' || chld == '4') {
      NS_WARNING("The value of command [AT+CHLD] is not supported");
      valid = false;
    } else if (chld == '1') {
      NotifyDialer(NS_LITERAL_STRING("CHUP+ATA"));
    } else if (chld == '2') {
      NotifyDialer(NS_LITERAL_STRING("CHLD+ATA"));
    } else {
      NS_WARNING("Wrong value of command [AT+CHLD]");
      valid = false;
    }

    if (!valid) {
      SendLine("ERROR");
      return;
    }
  } else if (msg.Find("AT+VGS=") != -1) {
    // Adjust volume by headset
    mReceiveVgsFlag = true;
    ParseAtCommand(msg, 7, atCommandValues);

    if (atCommandValues.IsEmpty()) {
      NS_WARNING("Could't get the value of command [AT+VGS=]");
      goto respond_with_ok;
    }

    nsresult rv;
    int newVgs = atCommandValues[0].ToInteger(&rv);
    if (NS_FAILED(rv)) {
      NS_WARNING("Failed to extract volume value from bluetooth headset!");
      goto respond_with_ok;
    }

    if (newVgs == mCurrentVgs) {
      goto respond_with_ok;
    }

#ifdef DEBUG
    NS_ASSERTION(newVgs >= 0 && newVgs <= 15, "Received invalid VGS value");
#endif

    nsString data;
    nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
    data.AppendInt(newVgs);
    os->NotifyObservers(nullptr, "bluetooth-volume-change", data.get());
  } else if (msg.Find("AT+BLDN") != -1) {
    NotifyDialer(NS_LITERAL_STRING("BLDN"));
  } else if (msg.Find("ATA") != -1) {
    NotifyDialer(NS_LITERAL_STRING("ATA"));
  } else if (msg.Find("AT+CHUP") != -1) {
    NotifyDialer(NS_LITERAL_STRING("CHUP"));
  } else if (msg.Find("ATD>") != -1) {
    // Currently, we don't support memory dialing in Dialer app
    SendLine("ERROR");
    return;
  } else if (msg.Find("ATD") != -1) {
    nsAutoCString message(msg), newMsg;
    int end = message.FindChar(';');
    if (end < 0) {
      NS_WARNING("Could't get the value of command [ATD]");
      goto respond_with_ok;
    }

    newMsg += nsDependentCSubstring(message, 0, end);
    NotifyDialer(NS_ConvertUTF8toUTF16(newMsg));
  } else if (msg.Find("AT+CLIP=") != -1) {
    ParseAtCommand(msg, 8, atCommandValues);

    if (atCommandValues.IsEmpty()) {
      NS_WARNING("Could't get the value of command [AT+CLIP=]");
      goto respond_with_ok;
    }

    mCLIP = (atCommandValues[0].EqualsLiteral("1"));
  } else if (msg.Find("AT+CKPD") != -1) {
    BluetoothScoManager* sco = BluetoothScoManager::Get();
    if (!sco) {
      NS_WARNING("Couldn't get BluetoothScoManager instance");
      goto respond_with_ok;
    }

    if (!sStopSendingRingFlag) {
      // Bluetooth HSP spec 4.2.2
      // There is an incoming call, notify Dialer to pick up the phone call
      // and SCO will be established after we get the CallStateChanged event
      // indicating the call is answered successfully.
      NotifyDialer(NS_LITERAL_STRING("ATA"));
    } else {
      if (!sco->IsConnected()) {
        // Bluetooth HSP spec 4.3
        // If there's no SCO, set up a SCO link.
        nsAutoString address;
        mSocket->GetAddress(address);
        sco->Connect(address);
      } else if (!mFirstCKPD) {
        // Bluetooth HSP spec 4.5
        // There are two ways to release SCO: sending CHUP to dialer or closing
        // SCO socket directly. We notify dialer only if there is at least one
        // active call.
        if (mCurrentCallStateArray.Length() > 1) {
          NotifyDialer(NS_LITERAL_STRING("CHUP"));
        } else {
          sco->Disconnect();
        }
      } else {
        // Three conditions have to be matched to come in here:
        // (1) Not sending RING indicator
        // (2) A SCO link exists
        // (3) This is the very first AT+CKPD=200 of this session
        // It is the case of Figure 4.3, Bluetooth HSP spec. Do nothing.
        NS_WARNING("AT+CKPD=200: Do nothing");
      }
    }

    mFirstCKPD = false;
  } else if (msg.Find("AT+CNUM") != -1) {
    if (!mMsisdn.IsEmpty()) {
      nsAutoCString message("+CNUM: ,\"");
      message.Append(NS_ConvertUTF16toUTF8(mMsisdn).get());
      message.AppendLiteral("\",");
      message.AppendInt(TOA_UNKNOWN);
      message.AppendLiteral(",,4");
      SendLine(message.get());
    }
  } else {
#ifdef DEBUG
    nsCString warningMsg;
    warningMsg.Append(NS_LITERAL_CSTRING("Unsupported AT command: "));
    warningMsg.Append(msg);
    warningMsg.Append(NS_LITERAL_CSTRING(", reply with ERROR"));
    NS_WARNING(warningMsg.get());
#endif
    SendLine("ERROR");
    return;
  }

respond_with_ok:
  // We always respond to remote device with "OK" in general cases.
  SendLine("OK");
}

bool
BluetoothHfpManager::Connect(const nsAString& aDevicePath,
                             const bool aIsHandsfree,
                             BluetoothReplyRunnable* aRunnable)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (gInShutdown) {
    NS_WARNING("Connect called while in shutdown!");
    return false;
  }

  if (mSocket) {
    NS_WARNING("BluetoothHfpManager has been already connected");
    return false;
  }

  // Stop listening because currently we only support one connection at a time.
  if (mHandsfreeSocket) {
    mHandsfreeSocket->Disconnect();
    mHandsfreeSocket = nullptr;
  }

  if (mHeadsetSocket) {
    mHeadsetSocket->Disconnect();
    mHeadsetSocket = nullptr;
  }

  BluetoothService* bs = BluetoothService::Get();
  NS_ENSURE_TRUE(bs, false);

  nsString uuid;
  if (aIsHandsfree) {
    BluetoothUuidHelper::GetString(BluetoothServiceClass::HANDSFREE, uuid);
  } else {
    BluetoothUuidHelper::GetString(BluetoothServiceClass::HEADSET, uuid);
  }

  mRunnable = aRunnable;
  mSocket =
    new BluetoothSocket(this, BluetoothSocketType::RFCOMM, true, true);

  nsresult rv = bs->GetSocketViaService(aDevicePath,
                                        uuid,
                                        BluetoothSocketType::RFCOMM,
                                        true,
                                        true,
                                        mSocket,
                                        mRunnable);

  return NS_FAILED(rv) ? false : true;
}

bool
BluetoothHfpManager::Listen()
{
  MOZ_ASSERT(NS_IsMainThread());

  if (gInShutdown) {
    MOZ_ASSERT(false, "Listen called while in shutdown!");
    return false;
  }

  if (mSocket) {
    NS_WARNING("mSocket exists. Failed to listen.");
    return false;
  }

  if (!mHandsfreeSocket) {
    mHandsfreeSocket =
      new BluetoothSocket(this, BluetoothSocketType::RFCOMM, true, true);

    if (!mHandsfreeSocket->Listen(
          BluetoothReservedChannels::CHANNEL_HANDSFREE_AG)) {
      NS_WARNING("[HFP] Can't listen on RFCOMM socket!");
      mHandsfreeSocket = nullptr;
      return false;
    }
  }

  if (!mHeadsetSocket) {
    mHeadsetSocket =
      new BluetoothSocket(this, BluetoothSocketType::RFCOMM, true, true);

    if (!mHeadsetSocket->Listen(
          BluetoothReservedChannels::CHANNEL_HEADSET_AG)) {
      NS_WARNING("[HSP] Can't listen on RFCOMM socket!");
      mHandsfreeSocket->Disconnect();
      mHandsfreeSocket = nullptr;
      mHeadsetSocket = nullptr;
      return false;
    }
  }

  return true;
}

void
BluetoothHfpManager::Disconnect()
{
  if (mSocket) {
    mSocket->Disconnect();
    mSocket = nullptr;
  }
}

bool
BluetoothHfpManager::SendLine(const char* aMessage)
{
  MOZ_ASSERT(mSocket);

  nsAutoCString msg;

  msg.AppendLiteral(kHfpCrlf);
  msg.Append(aMessage);
  msg.AppendLiteral(kHfpCrlf);

  return mSocket->SendSocketData(msg);
}

bool
BluetoothHfpManager::SendCommand(const char* aCommand, int aValue)
{
  if (!IsConnected()) {
    NS_WARNING("Trying to SendCommand() without a SLC");
    return false;
  }

  nsAutoCString message;
  int value = aValue;
  message += aCommand;

  if (!strcmp(aCommand, "+CIEV: ")) {
    if ((aValue < 1) || (aValue > ArrayLength(sCINDItems) - 1)) {
      NS_WARNING("unexpected CINDType for CIEV command");
      return false;
    }

    message.AppendInt(aValue);
    message.AppendLiteral(",");
    message.AppendInt(sCINDItems[aValue].value);
  } else if (!strcmp(aCommand, "+CIND: ")) {
    if (!aValue) {
      for (uint8_t i = 1; i < ArrayLength(sCINDItems); i++) {
        message.AppendLiteral("(\"");
        message.Append(sCINDItems[i].name);
        message.AppendLiteral("\",(");
        message.Append(sCINDItems[i].range);
        message.AppendLiteral(")");
        if (i == (ArrayLength(sCINDItems) - 1)) {
          message.AppendLiteral(")");
          break;
        }
        message.AppendLiteral("),");
      }
    } else {
      for (uint8_t i = 1; i < ArrayLength(sCINDItems); i++) {
        message.AppendInt(sCINDItems[i].value);
        if (i == (ArrayLength(sCINDItems) - 1)) {
          break;
        }
        message.AppendLiteral(",");
      }
    }
  } else {
    message.AppendInt(value);
  }

  return SendLine(message.get());
}

void
BluetoothHfpManager::SetupCIND(int aCallIndex, int aCallState,
                               const char* aNumber, bool aInitial)
{
  nsRefPtr<nsRunnable> sendRingTask;
  nsString address;

  while (aCallIndex >= (int)mCurrentCallStateArray.Length()) {
    mCurrentCallStateArray.AppendElement((int)nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED);
  }

  int currentCallState = mCurrentCallStateArray[aCallIndex];

  switch (aCallState) {
    case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
      sCINDItems[CINDType::CALLSETUP].value = CallSetupState::INCOMING;
      if (!aInitial) {
        SendCommand("+CIEV: ", CINDType::CALLSETUP);
      }

      if (!mCurrentCallIndex) {
        // Start sending RING indicator to HF
        sStopSendingRingFlag = false;

        if (!mCLIP) {
          MessageLoop::current()->
            PostDelayedTask(FROM_HERE,
                            new SendRingIndicatorTask(""),
                            sRingInterval);
        } else {
          // Same logic as implementation in ril_worker.js
          int type = TOA_UNKNOWN;

          if (aNumber && strlen(aNumber) > 0 && aNumber[0] == '+') {
            type = TOA_INTERNATIONAL;
          }

          MessageLoop::current()->
            PostDelayedTask(FROM_HERE,
                            new SendRingIndicatorTask(aNumber, type),
                            sRingInterval);
        }
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_DIALING:
      sCINDItems[CINDType::CALLSETUP].value = CallSetupState::OUTGOING;
      if (!aInitial) {
        SendCommand("+CIEV: ", CINDType::CALLSETUP);

        mSocket->GetAddress(address);
        OpenScoSocket(address);
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_ALERTING:
      sCINDItems[CINDType::CALLSETUP].value = CallSetupState::OUTGOING_ALERTING;
      if (!aInitial) {
        SendCommand("+CIEV: ", CINDType::CALLSETUP);
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_CONNECTED:
      mCurrentCallIndex = aCallIndex;
      if (aInitial) {
        sCINDItems[CINDType::CALL].value = CallState::IN_PROGRESS;
        sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
      } else {
        switch (currentCallState) {
          case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
            // Incoming call, no break
            sStopSendingRingFlag = true;

            mSocket->GetAddress(address);
            OpenScoSocket(address);
          case nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED:
          case nsIRadioInterfaceLayer::CALL_STATE_ALERTING:
            // Outgoing call
            sCINDItems[CINDType::CALL].value = CallState::IN_PROGRESS;
            SendCommand("+CIEV: ", CINDType::CALL);
            sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
            SendCommand("+CIEV: ", CINDType::CALLSETUP);
            break;
          default:
#ifdef DEBUG
            NS_WARNING("Not handling state changed");
#endif
            break;
        }
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED:
      if (!aInitial) {
        switch (currentCallState) {
          case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
          case nsIRadioInterfaceLayer::CALL_STATE_BUSY:
            // Incoming call, no break
            sStopSendingRingFlag = true;
          case nsIRadioInterfaceLayer::CALL_STATE_DIALING:
          case nsIRadioInterfaceLayer::CALL_STATE_ALERTING:
            // Outgoing call
            sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
            SendCommand("+CIEV: ", CINDType::CALLSETUP);
            break;
          case nsIRadioInterfaceLayer::CALL_STATE_CONNECTED:
            sCINDItems[CINDType::CALL].value = CallState::NO_CALL;
            SendCommand("+CIEV: ", CINDType::CALL);
            break;
          case nsIRadioInterfaceLayer::CALL_STATE_HELD:
            sCINDItems[CINDType::CALLHELD].value = NO_CALLHELD;
            SendCommand("+CIEV: ", CINDType::CALLHELD);
          default:
#ifdef DEBUG
            NS_WARNING("Not handling state changed");
#endif
            break;
        }

        if (aCallIndex == mCurrentCallIndex) {
#ifdef DEBUG
          NS_ASSERTION(mCurrentCallStateArray.Length() > aCallIndex,
            "Call index out of bounds!");
#endif
          mCurrentCallStateArray[aCallIndex] = aCallState;

          // Find the first non-disconnected call (like connected, held),
          // and update mCurrentCallIndex
          int c;
          for (c = 1; c < (int)mCurrentCallStateArray.Length(); c++) {
            if (mCurrentCallStateArray[c] != nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED) {
              mCurrentCallIndex = c;
              break;
            }
          }

          // There is no call
          if (c == (int)mCurrentCallStateArray.Length()) {
            mCurrentCallIndex = 0;
            CloseScoSocket();
          }
        }
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_HELD:
      sCINDItems[CINDType::CALLHELD].value = CallHeldState::ONHOLD_ACTIVE;

      if (!aInitial) {
        SendCommand("+CIEV: ", CINDType::CALLHELD);
      }

      break;
    default:
#ifdef DEBUG
      NS_WARNING("Not handling state changed");
      sCINDItems[CINDType::CALL].value = CallState::NO_CALL;
      sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
      sCINDItems[CINDType::CALLHELD].value = CallHeldState::NO_CALLHELD;
#endif
      break;
  }

  mCurrentCallStateArray[aCallIndex] = aCallState;
}

/*
 * EnumerateCallState will be called for each call
 */
void
BluetoothHfpManager::EnumerateCallState(int aCallIndex, int aCallState,
                                        const char* aNumber, bool aIsActive)
{
  SetupCIND(aCallIndex, aCallState, aNumber, true);

  if (sCINDItems[CINDType::CALL].value == CallState::IN_PROGRESS ||
      sCINDItems[CINDType::CALLSETUP].value == CallSetupState::OUTGOING ||
      sCINDItems[CINDType::CALLSETUP].value == CallSetupState::OUTGOING_ALERTING) {
    nsString address;
    mSocket->GetAddress(address);
    OpenScoSocket(address);
  }
}

/*
 * CallStateChanged will be called whenever call status is changed, and it
 * also means we need to notify HS about the change. For more information, 
 * please refer to 4.13 ~ 4.15 in Bluetooth hands-free profile 1.6.
 */
void
BluetoothHfpManager::CallStateChanged(int aCallIndex, int aCallState,
                                      const char* aNumber, bool aIsActive)
{
  if (!IsConnected()) {
    return;
  }

  SetupCIND(aCallIndex, aCallState, aNumber, false);
}

void
BluetoothHfpManager::OnConnectSuccess(BluetoothSocket* aSocket)
{
  MOZ_ASSERT(aSocket);

  /**
   * If the created connection is an inbound connection, close another server
   * socket because currently only one SLC is allowed. After that, we need to
   * make sure that both server socket would be nulled out. As for outbound
   * connections, we do nothing since sockets have been already handled in
   * function Connect().
   */
  if (aSocket == mHandsfreeSocket) {
    MOZ_ASSERT(!mSocket);
    mHandsfreeSocket.swap(mSocket);

    mHeadsetSocket->Disconnect();
    mHeadsetSocket = nullptr;
  } else if (aSocket == mHeadsetSocket) {
    MOZ_ASSERT(!mSocket);
    mHeadsetSocket.swap(mSocket);

    mHandsfreeSocket->Disconnect();
    mHandsfreeSocket = nullptr;
  }

  // For active connection request, we need to reply the DOMRequest
  if (mRunnable) {
    BluetoothValue v = true;
    nsString errorStr;
    DispatchBluetoothReply(mRunnable, v, errorStr);

    mRunnable.forget();
  }

  mFirstCKPD = true;

  // Cache device path for NotifySettings() since we can't get socket address
  // when a headset disconnect with us
  mSocket->GetAddress(mDevicePath);

  nsCOMPtr<nsIRILContentHelper> ril =
    do_GetService(NS_RILCONTENTHELPER_CONTRACTID);
  NS_ENSURE_TRUE_VOID(ril);
  ril->EnumerateCalls(mListener->GetCallback());

  NotifySettings();
}

void
BluetoothHfpManager::OnConnectError(BluetoothSocket* aSocket)
{
  // For active connection request, we need to reply the DOMRequest
  if (mRunnable) {
    BluetoothValue v;
    nsString errorStr;
    errorStr.AssignLiteral("Failed to connect with a bluetooth headset!");
    DispatchBluetoothReply(mRunnable, v, errorStr);

    mRunnable.forget();
  }

  mSocket = nullptr;
  mHandsfreeSocket = nullptr;
  mHeadsetSocket = nullptr;

  // If connecting for some reason didn't work, restart listening
  Listen();
}

void
BluetoothHfpManager::OnDisconnect(BluetoothSocket* aSocket)
{
  MOZ_ASSERT(aSocket);

  if (aSocket != mSocket) {
    // Do nothing when a listening server socket is closed.
    return;
  }

  mSocket = nullptr;
  CloseScoSocket();

  sStopSendingRingFlag = true;
  sCINDItems[CINDType::CALL].value = CallState::NO_CALL;
  sCINDItems[CINDType::CALLSETUP].value = CallSetupState::NO_CALLSETUP;
  sCINDItems[CINDType::CALLHELD].value = CallHeldState::NO_CALLHELD;
  mCLIP = false;

  Listen();
  NotifySettings();
}

bool
BluetoothHfpManager::IsConnected()
{
  if (mSocket) {
    return mSocket->GetConnectionStatus() ==
           SocketConnectionStatus::SOCKET_CONNECTED;
  }

  return false;
}

