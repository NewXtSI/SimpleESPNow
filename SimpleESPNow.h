#ifndef INCLUDE_SIMPLEESPNOW_INCLUDED
#define INCLUDE_SIMPLEESPNOW_INCLUDED

#include <Arduino.h>
#include <esp_now.h>

#define MAX_MESSAGES_IN_QUEUE    20

class SimpleESPNowPeer {
 public:
    uint8_t             mac[6] = {0, 0, 0, 0, 0, 0};
    char                name[16] = {'\0'};
 private:
};

class SimpleESPPlatformMsg {
 public:
    uint8_t             mac[6];
    uint16_t            msgID;
    uint8_t             data[ESP_NOW_MAX_DATA_LEN];
    int                 dataLen;
    bool                waitingForAck = false;
    uint32_t            timeStamp;
 private:
};

class SimpleESPNow {
 public:
                        SimpleESPNow();
    void                init(char *name);
    void                begin();
    uint16_t            send(SimpleESPNowPeer *peer, const uint8_t *data, int dataLen, bool requestAck = false, bool qos = false);
    void                setOnReceive(std::function<void(SimpleESPNowPeer *peer, const uint8_t *data, int dataLen)> onReceive) {
       this->onReceive = onReceive;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 1)
    void                RecvCallback(const esp_now_recv_info_t * esp_now_info, const uint8_t *data, int dataLen);
#else
    void                 RecvCallback(const unsigned char *macAddr, const uint8_t *data, int dataLen);
#endif
    void                 SendCallback(const unsigned char *macAddr, esp_now_send_status_t status);
    SimpleESPNowPeer    *getSelf() {
        return &self;
    }
    SimpleESPNowPeer    *getBroadcast() {
       return &broadcast;
    }
    SimpleESPNowPeer    *getPeer(const uint8_t *mac);
    SimpleESPNowPeer    *getPeerByName(const char *name);
    void                 sendTimeSync();
    uint32_t             millis();
    bool                 isTimeSynced() { return timeSynced; }
 private:
    std::vector<SimpleESPPlatformMsg*> messages;
    std::vector<SimpleESPNowPeer*> peers;
    std::function<void(SimpleESPNowPeer *peer, const uint8_t *data, int dataLen)> onReceive = nullptr;
    bool                initialized = false;
    bool                isSending = false;
    uint8_t             channel = 2;                // WiFi channel
    uint8_t             static_rx_buf_num = 2;
    uint8_t             dynamic_rx_buf_num = 4;
    uint8_t             static_tx_buf_num = 2;
    SimpleESPNowPeer    self;
    SimpleESPNowPeer    broadcast;

    SimpleESPNowPeer    *addPeer(const uint8_t *mac, const char *name);
    SimpleESPNowPeer    *addPeer(const uint8_t *mac);
    SimpleESPNowPeer    *addPeer();
    void                setPeerName(SimpleESPNowPeer *peer, const char *name);
    uint16_t            _send(const uint8_t *mac, const uint8_t *data, int dataLen);
    void                _sendFromQueue();
    bool                peerHasKnownName(const uint8_t *mac);
    void                sendNameRequest(const uint8_t *mac);
    uint16_t            msgCounter = 0;
    int32_t             timeOffset = 0;
    bool                timeSynced = false;
    bool                isTimeSyncMaster = false;
    uint32_t            lastTimeSync = 0;

    void                checkTimeSync();
};

extern SimpleESPNow simpleESPNow;

#endif  /* INCLUDE_SIMPLEESPNOW_INCLUDED */
