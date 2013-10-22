/*
 * Receive firefox-accounts messages from Identity service and reflect
 * them up to gaia system content using mozChromeEvents.
 *
 * For every message sent to content, expect a response back in the form
 * of a mozContentEvent.
 *
 * The mozContentEvent detail should contain the uuid of the mozChromeEvent.
 *
 * Messages sent to content are objects of the form:
 *
 * {
 *   type:                  'identity-firefox-accounts-watch' or
 *                          'identity-firefox-accounts-request' or
 *                          'identity-firefox-accounts-logout'
 *   rpid:                  unique id of RP caller
 *                          XXX rpid is unnecessary for content?
 *   uuid:                  unique message id
 *   appStatus:             status of DOM caller nodePrincipal
 * }
 *
 * Returned messages should be objects of the form:
 *
 * {
 *   method:                'ready', 'login', 'logout', 'cancel'
 *   assertion:             required for 'login' messages; otherwise ignored
 *   uuid:                  the uuid from the initial chrome event
 * }
 *
 * Upon receipt of a content message, if it contains a valid uuid,
 * call the specified method on the Identity Service for the original
 * RP caller
 */

"use strict";

this.EXPORTED_SYMBOLS = ["SignInToFXA"];

const Ci = Components.interfaces;
const Cu = Components.utils;

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "IdentityService",
                                  "resource://gre/modules/identity/MinimalIdentity.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "Logger",
                                  "resource://gre/modules/identity/LogUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "getRandomId",
                                  "resource://gre/modules/identity/IdentityUtils.jsm");

function log(...aMessageArgs) {
  Logger.log.apply(Logger, ["SignInToFXA"].concat(aMessageArgs));
}

function reportError(...aMessageArgs) {
  Logger.reportError.apply(Logger, ["SignInToFXA"].concat(aMessageArgs));
}

this.SignInToFXA = {
  callers: {},

  init: function() {
    log("** FXA init");
    // Note that we don't care about the unwatch event that the persona
    // system listens for.  This is because we are not in a separate process
    // that has to be closed when there are no operative flows.
    Services.obs.addObserver(this, "identity-firefox-accounts-watch", false);
    Services.obs.addObserver(this, "identity-firefox-accounts-request", false);
    Services.obs.addObserver(this, "identity-firefox-accounts-logout", false);
  },

  uninit: function() {
    Services.obs.removeObserver(this, "identity-firefox-accounts-watch");
    Services.obs.removeObserver(this, "identity-firefox-accounts-request");
    Services.obs.removeObserver(this, "identity-firefox-accounts-logout");
  },

  /*
   * Mediate a request from a DOM caller for the System app to perform
   * a Firefox Accounts RP API call.  Like Persona, Firefox Accounts uses
   * the BrowserID protocol.
   */
  observe: function(aSubject, aTopic, aData) {
    let options;
    if (aSubject) {
      options = aSubject.wrappedJSObject;
    }

    if (!(options && options.id)) {
      return reportError("Received", aTopic, "with invalid or no options");
    }

    let browser = Services.wm.getMostRecentWindow("navigator:browser");
    let content = browser.getContentWindow();
    let uuid = getRandomId();

    // Record the RP caller for this request
    this.callers[uuid] = options.id;

    // Prepare the detail of a mozChromeEvent
    let detail = {
      type: aTopic,
      rpid: options.id,
      uuid: uuid,
      appStatus: options.appStatus
    };

    log("Sending chrome event with detail: " + JSON.stringify(detail));

    // Prepare to receive a response from content for our chrome event.
    // If the uuid of the received event matches that of our detail,
    // Remove the event listener and fire the requested method on the
    // Identity Service.
    //
    // For example, if we emit a "request" event, we will receive "login"
    // or "cancel" back as the requested method to invoke.
    content.addEventListener("mozContentEvent", (evt) => {
      let msg = evt.detail;
      log("Received a mozContentEvent with uuid " + msg.uuid);
      if (msg.uuid && this.callers[msg.uuid]) {
        content.removeEventListener("mozContentEvent", gotContentEvent);

        let rpId = this.callers[msg.uuid];
        delete this.callers[msg.uuid];

        log("Received a mozContentEvent for RP ID " + rpId);
        log("Message contents: " + JSON.stringify(msg));

        switch (msg.method) {
          case "ready":
            IdentityService.doReady(rpId);
            break;

          case "login":
            if (typeof msg.asssertion === 'undefined') {
              reportError("Message missing 'assertion' property");
            }
            IdentityService.doLogin(rpId, msg.assertion);
            break;

          // XXX does FXA support this?
          case "logout":
            IdentityService.doLogout(rpId);
            break;

          // XXX does FXA support this?
          case "cancel":
            IdentityService.doCancel(rpId);
            break;

          default:
            reportError("Unsupported method:", msg.method);
            break;
        }
      }
    });

    log("** sending detail...");
    browser.shell.sendChromeEvent(detail);
  }
};
