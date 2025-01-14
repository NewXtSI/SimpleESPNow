#include "SimpleESPNow.h"
#include "esp_wifi.h"
#include <esp_now.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 1)
#include "esp_mac.h"
#endif

#define ESP32DEBUGGING
#include "esp32logger.h"

#define EXTRA_HEADER_LENGTH 4

// Internal data messagetypes
#define SYSTEM_MESSAGE      0x00
#define PING_MSG            0x01
#define PONG_MSG            0x02
#define ACK_MSG             0x03
#define NAME_REQUEST_MSG    0x04
#define NAME_RESPONSE_MSG   0x05
#define TIME_SYNC_MSG       0x06

#define DATA_MSG            0xAC


#define MSG_HEADER_ID_TYPE  0x00
#define MSG_HEADER_ID_FLAGS 0x01
#define MSG_HEADER_ID_NUM1  0x02
#define MSG_HEADER_ID_NUM2  0x03

#define MSG_FLAG_SEND_QOS   _BV(0)
#define MSG_FLAG_REQ_ACK    _BV(1)

SimpleESPNowPeer*
SimpleESPNow::getPeerByName(const char *name) {
    SimpleESPNowPeer *ret = nullptr;
    for (auto peer : peers) {
        if (strcmp(peer->name, name) == 0) {
            ret = peer;
            break;
        }
    }
    return ret;
}

SimpleESPNowPeer*
SimpleESPNow::getPeer(const uint8_t *mac) {
    SimpleESPNowPeer *ret = nullptr;
    for (auto peer : peers) {
        if (memcmp(peer->mac, mac, 6) == 0) {
            ret = peer;
            break;
        }
    }
    return ret;
}

void
SimpleESPNow::setPeerName(SimpleESPNowPeer *peer, const char *name) {
    snprintf(peer->name, sizeof(peer->name), "%s", name);
}

SimpleESPNowPeer    *
SimpleESPNow::addPeer(const uint8_t *mac, const char *name) {
    SimpleESPNowPeer    *peer = addPeer();
    memcpy(peer->mac, mac, 6);
    setPeerName(peer, name);
    return peer;
}

SimpleESPNowPeer    *
SimpleESPNow::addPeer(const uint8_t *mac) {
    SimpleESPNowPeer    *peer = addPeer();
    memcpy(peer->mac, mac, 6);
    return peer;
}

SimpleESPNowPeer*
SimpleESPNow::addPeer() {
    SimpleESPNowPeer    *peer = new SimpleESPNowPeer();
    peers.push_back(peer);
    return peer;
}

SimpleESPNow::SimpleESPNow() {
}

void
SimpleESPNow::begin() {
    if (!initialized) {
        DBGLOG(Error, "SimpleESPNow not initialized");
        return;
    }
    DBGLOG(Verbose, "Starting SimpleESPNow");
    xTaskCreate([](void *arg) {
        SimpleESPNow *self = (SimpleESPNow*)arg;
        uint32_t syncCheck = 0;
        DBGLOG(Verbose, "SimpleESPNow Task started");
        while (true) {
            self->_sendFromQueue();
            vTaskDelay(1);
            if (syncCheck % 1000 == 0) {
                self->checkTimeSync();
                self->checkPeers();
            }
            syncCheck++;
        }
    }, "SimpleESPNow", 4096, this, 3, NULL);
}

#define ESPNOW_WIFI_MODE    WIFI_MODE_STA
#define ESPNOW_WIFI_IF      ESP_IF_WIFI_STA
#define ESPNOW_WIFI_CHANNEL 1

// typedef void (*esp_now_send_cb_t)(const uint8_t *mac_addr, esp_now_send_status_t status);
void SimpleESPNowSendCallback(const unsigned char *macAddr, esp_now_send_status_t status) {
    simpleESPNow.SendCallback(macAddr, status);
}

// typedef void (*esp_now_recv_cb_t)(const uint8_t *mac_addr, const uint8_t *data, int data_len);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 1)
void SimpleESPNowRecvCallback(const esp_now_recv_info_t * esp_now_info, const uint8_t *data, int dataLen) {
    simpleESPNow.RecvCallback(esp_now_info, data, dataLen);
}
#else
void SimpleESPNowRecvCallback(const unsigned char *macAddr, const unsigned char *data, int dataLen) {

    simpleESPNow.RecvCallback(macAddr, data, dataLen);
}
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 1)
void
SimpleESPNow::RecvCallback(const esp_now_recv_info_t * esp_now_info, const uint8_t *data, int dataLen) {
    unsigned char *macAddr = esp_now_info->src_addr;
#else
void
SimpleESPNow::RecvCallback(const unsigned char *macAddr, const uint8_t *data, int dataLen) {
#endif
    bool bIsSystemMessage = false;
    // Check, if peer exists
    SimpleESPNowPeer *peer = getPeer(macAddr);
    if (peer == nullptr) {
        DBGLOG(Verbose, "New peer detected");
        peer = addPeer(macAddr);
    }
    if (dataLen < EXTRA_HEADER_LENGTH) {
        DBGLOG(Error, "Invalid message length");
        // Error, BasisInfo fehlt
        return;
    }
    peer->lastSeen = ::millis();
    // Check, if is system message
    // First, check for ACKs
    if (data[MSG_HEADER_ID_TYPE] == ACK_MSG) {
        // ACK
        for (auto msg : messages) {
            if (msg->data[MSG_HEADER_ID_NUM1] == data[MSG_HEADER_ID_NUM1] && msg->data[MSG_HEADER_ID_NUM2] == data[MSG_HEADER_ID_NUM2]) {
                msg->waitingForAck = false;
                DBGLOG(Verbose, "ACK received for %02X:%02X", data[MSG_HEADER_ID_NUM1], data[MSG_HEADER_ID_NUM2]);
                messages.erase(std::remove(messages.begin(), messages.end(), msg), messages.end());
                delete msg;
                return;
            }
        }
        DBGLOG(Verbose, "ACK received for unknown message %02X:%02X", data[MSG_HEADER_ID_NUM1], data[MSG_HEADER_ID_NUM2]);
        return;
    }
    // Auto ACK
    if (data[MSG_HEADER_ID_FLAGS] & MSG_FLAG_REQ_ACK) {
        DBGLOG(Verbose, "ACK requested, sending for MessageID %02X %02X", data[MSG_HEADER_ID_NUM1], data[MSG_HEADER_ID_NUM2]);
        // ACK Request
        uint8_t ackData[EXTRA_HEADER_LENGTH];
        ackData[MSG_HEADER_ID_TYPE] = ACK_MSG;
        ackData[MSG_HEADER_ID_NUM1] = data[MSG_HEADER_ID_NUM1];
        ackData[MSG_HEADER_ID_NUM2] = data[MSG_HEADER_ID_NUM2];
        _send(macAddr, ackData, EXTRA_HEADER_LENGTH);
    }
    if (!peerHasKnownName(macAddr)) {
        DBGLOG(Verbose, "Requesting name from %02X:%02X:%02X:%02X:%02X:%02X", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
        sendNameRequest(macAddr);
    }
    if (data[MSG_HEADER_ID_TYPE] == PONG_MSG) {
        DBGLOG(Verbose, "Pong Message received from %s", peer->name);
        bIsSystemMessage = true;
    }
    if (data[MSG_HEADER_ID_TYPE] == PING_MSG) {
        DBGLOG(Verbose, "Ping Message received from %s", peer->name);
        // Ping
        uint8_t sendBuf[EXTRA_HEADER_LENGTH];
        sendBuf[MSG_HEADER_ID_TYPE] = PONG_MSG;
        sendBuf[MSG_HEADER_ID_FLAGS] = MSG_FLAG_SEND_QOS;
        _send(macAddr, sendBuf, EXTRA_HEADER_LENGTH);
        return;
    }
    if (data[MSG_HEADER_ID_TYPE] == NAME_REQUEST_MSG) {
        DBGLOG(Verbose, "Name Request Message received from %s", peer->name);
        // Name Request
        uint8_t sendBuf[EXTRA_HEADER_LENGTH+10];
        sendBuf[MSG_HEADER_ID_TYPE] = NAME_RESPONSE_MSG;
        sendBuf[MSG_HEADER_ID_FLAGS] = MSG_FLAG_SEND_QOS;
        memcpy(sendBuf+EXTRA_HEADER_LENGTH, self.name, 10);
        _send(macAddr, sendBuf, EXTRA_HEADER_LENGTH+10);
        return;
    }
    if (data[MSG_HEADER_ID_TYPE] == NAME_RESPONSE_MSG) {
        DBGLOG(Verbose, "Name Response Message received from %s", peer->name);
        // Name Response
        memcpy(peer->name, data+EXTRA_HEADER_LENGTH, 10);
        return;
    }
    if (data[MSG_HEADER_ID_TYPE] == TIME_SYNC_MSG) {
        DBGLOG(Verbose, "Time Sync Message received from %s", peer->name);
        // Time Sync
        uint32_t time = data[EXTRA_HEADER_LENGTH] | (data[EXTRA_HEADER_LENGTH+1] << 8) | (data[EXTRA_HEADER_LENGTH+2] << 16) | (data[EXTRA_HEADER_LENGTH+3] << 24);
        uint32_t now = ::millis();
        timeOffset = time - now;
        timeSynced = true;
        lastTimeSync = now;
        isTimeSyncMaster = false;
        return;
    }
    // Transfer message to class callback
    if (!bIsSystemMessage && onReceive != nullptr) {
        onReceive(peer, data+EXTRA_HEADER_LENGTH, dataLen-EXTRA_HEADER_LENGTH);
    }
}

uint32_t
SimpleESPNow::millis() {
    return ::millis() + timeOffset;
}

void
SimpleESPNow::sendNameRequest(const uint8_t *mac) {
    if (mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF && mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF) {
        return;
    }
    uint8_t sendBuf[EXTRA_HEADER_LENGTH];
    sendBuf[MSG_HEADER_ID_TYPE] = NAME_REQUEST_MSG;
    sendBuf[MSG_HEADER_ID_FLAGS] = MSG_FLAG_SEND_QOS;
    _send(mac, sendBuf, EXTRA_HEADER_LENGTH);
}

bool
SimpleESPNow::peerHasKnownName(const uint8_t *mac) {
    for (auto peer : peers) {
        if (memcmp(peer->mac, mac, 6) == 0) {
            DBGLOG(Verbose, "Peer %02X:%02X:%02X:%02X:%02X:%02X has name %s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], peer->name);
            return strlen(peer->name) > 0;
        }
    }
    return false;
}

void
SimpleESPNow::SendCallback(const unsigned char *macAddr, esp_now_send_status_t status) {
    if (messages.size() == 0) {
        return;
    }
    SimpleESPPlatformMsg *msg = messages[0];
    messages.erase(std::remove(messages.begin(), messages.end(), msg), messages.end());
    if (status != ESP_NOW_SEND_SUCCESS) {
        if ((msg->data[MSG_HEADER_ID_FLAGS] & MSG_FLAG_SEND_QOS) || (msg->data[MSG_HEADER_ID_FLAGS] & MSG_FLAG_REQ_ACK)) {
            // Resend, aber als letztes
            messages.push_back(msg);
        } else {
            delete msg;
        }
    } else {
        if (msg->waitingForAck) {
            // Resend, aber als letztes
            messages.push_back(msg);
        } else {
            if (msg->data[MSG_HEADER_ID_FLAGS] & MSG_FLAG_REQ_ACK) {
                msg->waitingForAck = true;
                messages.push_back(msg);
            } else {
                delete msg;
            }
        }
    }
    isSending = false;
}

void
SimpleESPNow::_sendFromQueue() {
    if (messages.size() == 0 || isSending) {
        return;
    }
    SimpleESPPlatformMsg *msg = messages[0];
    esp_now_peer_info_t peerInfo = {};
    memcpy(&peerInfo.peer_addr, msg->mac, 6);
    if (!esp_now_is_peer_exist(msg->mac)) {
        esp_now_add_peer(&peerInfo);
    }
    isSending = true;
    esp_err_t result = esp_now_send(msg->mac, msg->data, msg->dataLen);
}

uint16_t
SimpleESPNow::_send(const uint8_t *mac, const uint8_t *data, int dataLen) {
    DBGLOG(Verbose, "Sending %d bytes to %02X:%02X:%02X:%02X:%02X:%02X", dataLen, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    SimpleESPPlatformMsg *msg = new SimpleESPPlatformMsg();
    memcpy(msg->mac, mac, 6);
    memcpy(msg->data, data, dataLen);
    msg->dataLen = dataLen;
    msg->waitingForAck = false;
    msg->data[MSG_HEADER_ID_NUM1] = msgCounter & 0xFF;
    msg->data[MSG_HEADER_ID_NUM2] = (msgCounter >> 8) & 0xFF;
    msgCounter++;
    msg->timeStamp = this->millis();
    messages.push_back(msg);
    return msgCounter-1;
}

uint16_t
SimpleESPNow::send(SimpleESPNowPeer *peer, const uint8_t *data, int dataLen, bool requestAck, bool qos) {
    uint8_t sendBuf[ESP_NOW_MAX_DATA_LEN];
    memcpy(sendBuf+EXTRA_HEADER_LENGTH, data, dataLen);
    sendBuf[MSG_HEADER_ID_TYPE] = DATA_MSG;
    sendBuf[MSG_HEADER_ID_FLAGS] = 0x00;
    if (requestAck) {
        sendBuf[MSG_HEADER_ID_FLAGS] |= MSG_FLAG_REQ_ACK;
    }
    if (qos) {
        sendBuf[MSG_HEADER_ID_FLAGS] |= MSG_FLAG_SEND_QOS;
    }
    return _send(peer->mac, sendBuf, dataLen+EXTRA_HEADER_LENGTH);
}

void
SimpleESPNow::sendTimeSync() {

    uint8_t sendBuf[EXTRA_HEADER_LENGTH+4];
    uint32_t time = ::millis();
    timeOffset = 0;             // Setzen auf 0, da Master
    timeSynced = true;
    isTimeSyncMaster = true;
    lastTimeSync = time;

    sendBuf[MSG_HEADER_ID_TYPE] = TIME_SYNC_MSG;
    sendBuf[MSG_HEADER_ID_FLAGS] = 0x00;
    sendBuf[EXTRA_HEADER_LENGTH] = time & 0xFF;
    sendBuf[EXTRA_HEADER_LENGTH+1] = (time >> 8) & 0xFF;
    sendBuf[EXTRA_HEADER_LENGTH+2] = (time >> 16) & 0xFF;
    sendBuf[EXTRA_HEADER_LENGTH+3] = (time >> 24) & 0xFF;
    DBGLOG(Verbose, "Broadcasting time: %d", time);
    _send(broadcast.mac, sendBuf, EXTRA_HEADER_LENGTH+4);
}

void
SimpleESPNow::sendPing(SimpleESPNowPeer *peer) {
    uint8_t sendBuf[EXTRA_HEADER_LENGTH];
    sendBuf[MSG_HEADER_ID_TYPE] = PING_MSG;
    sendBuf[MSG_HEADER_ID_FLAGS] = MSG_FLAG_SEND_QOS;
    _send(peer->mac, sendBuf, EXTRA_HEADER_LENGTH);
}

void
SimpleESPNow::checkPeers() {
    for (auto peer : peers) {
        if (::millis() - peer->lastSeen > 20000) {
            DBGLOG(Verbose, "Peer %s not seen for 20 seconds, removing!", peer->name);
        } else  if (::millis() - peer->lastSeen > 10000) {
            DBGLOG(Verbose, "Peer %s not seen for 10 seconds, sending Ping", peer->name);
            sendPing(peer);

        }
    }
}

void
SimpleESPNow::checkTimeSync() {
//    if (!isTimeSyncMaster)
 //       sendTimeSync();
//    return;
    if (isTimeSyncMaster) {
        if (::millis() - lastTimeSync > 4000) {
            sendTimeSync();
        }
    } else {
    if (!timeSynced) {  // Wenn nach 5 Sekunden keiner den Sync übernommen hat, dann startet dieser Client
        if (::millis() - lastTimeSync > 5000 + random(300)) {
            sendTimeSync();
        }
    } else {
        if (::millis() - lastTimeSync > (60000 + random(500))) {  // War mal gesynct, aber es kam nichts mehr. Dann übernehmen
            sendTimeSync();
        }
    }
    }
}

void
SimpleESPNow::init(char *name) {
    if (initialized)
        return;

    // Fill self peer
    esp_read_mac(self.mac, ESP_MAC_WIFI_STA);
    snprintf(self.name, sizeof(self.name), "%s", name);
    // Fill broadcast peer
    broadcast.mac[0] = 0xFF;
    broadcast.mac[1] = 0xFF;
    broadcast.mac[2] = 0xFF;
    broadcast.mac[3] = 0xFF;
    broadcast.mac[4] = 0xFF;
    broadcast.mac[5] = 0xFF;
    setPeerName(&broadcast, "Broadcast");
    // Init ESPNow
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg =    WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num =     this->static_rx_buf_num;   // War 8
    cfg.dynamic_rx_buf_num =    this->dynamic_rx_buf_num;  // War 32
    cfg.static_tx_buf_num =     this->static_tx_buf_num;   // War 8

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(this->channel, WIFI_SECOND_CHAN_NONE));

    esp_now_init();
    // Register callback
    esp_now_register_recv_cb(SimpleESPNowRecvCallback);
    esp_now_register_send_cb(SimpleESPNowSendCallback);
    initialized = true;
}

SimpleESPNow simpleESPNow;