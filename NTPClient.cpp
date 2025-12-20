/* Copyright (c) 2019 ARM, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "NTPClient.h"
#include "mbed.h"

bool NTPClient::resolveDNS(const SntpServerInfo_t *pServerAddr, uint32_t *pIpV4Addr) {
    SocketAddress result;

    if(instance().interface->gethostbyname(pServerAddr->pServerName, &result, NSAPI_IPv4) != NSAPI_ERROR_OK) {
        return false;
    }

    memcpy(pIpV4Addr, result.get_ip_bytes(), sizeof(uint32_t));
    return true;
}

void NTPClient::getRTCTime(SntpTimestamp_t *pCurrentTime) {

    // Set seconds based on real-time clock. We need to convert from NTP epoch (Jan 1 1900) to
    // UNIX time (epoch = Jan 1 1970)
    uint32_t secsSince1970 = std::chrono::duration_cast<std::chrono::seconds>(RealTimeClock::now().time_since_epoch()).count();
    pCurrentTime->seconds = secsSince1970 + SNTP_TIME_AT_UNIX_EPOCH_SECS;

    // Unfortunately the Mbed RTC does not currently implement sub-second timing, so we have to leave the fractional part as 0.
    pCurrentTime->fractions = 0;
}

void NTPClient::saveTimeOffset(const SntpServerInfo_t *pTimeServer, const SntpTimestamp_t *pServerTime,
    int64_t clockOffsetMs, SntpLeapSecondInfo_t leapSecondInfo) {
    // Currently we don't use the time server addr or leap second info.
    (void)pTimeServer;
    (void)leapSecondInfo;

    // Also, the offset will generally provide a more accurate sync, and we aren't too worried about
    // running out of bits in a millisecond type, so we can throw out the server time
    (void)pServerTime;

    instance().last_time_offset.offset = std::chrono::milliseconds(clockOffsetMs);
}

int32_t NTPClient::udpSendto(NetworkContext_t *pNetworkContext, uint32_t serverAddr, uint16_t serverPort,
    const void *pBuffer, uint16_t bytesToSend) {
    (void)pNetworkContext;

    SocketAddress destAddr(&serverAddr, NSAPI_IPv4, serverPort);

    const auto ret = instance().socket.sendto(destAddr, pBuffer, bytesToSend);
    if(ret > 0) {
        // Connect the socket to this server so that we will receive a reply from it only and not other random
        // packets to this port
        instance().socket.connect(destAddr);

        return ret; // Ret is number of bytes transmitted
    }
    else if(ret == NSAPI_ERROR_WOULD_BLOCK) {
        return 0; // 0 indicates to coreSNTP that we couldn't send without blocking
    }
    else {
        // Other negative error code
        return ret;
    }
}

int32_t NTPClient::udpRecvfrom(NetworkContext_t *pNetworkContext, uint32_t serverAddr, uint16_t serverPort,
    void *pBuffer, uint16_t bytesToRecv) {
    (void)pNetworkContext;

    // We already connected the socket in udpSendto, so we don't need to do that here
    (void)serverAddr;
    (void)serverPort;

    const auto ret = instance().socket.recv(pBuffer, bytesToRecv);

    if(ret > 0) {
        // Return number of bytes received
        return ret;
    }
    else if(ret == NSAPI_ERROR_WOULD_BLOCK) {
        // Return 0 to indicate no data avail
        return 0;
    }
    else {
        // Negative error code
        return ret;
    }
}

NTPClient & NTPClient::instance() {
    static NTPClient instance;
    return instance;
}

SntpStatus_t NTPClient::init(NetworkInterface *interface, char const * const *ntp_servers, size_t num_ntp_servers) {

    if(interface == nullptr || ntp_servers == nullptr || num_ntp_servers == 0) {
        return SntpErrorBadParameter;
    }

    // Save data into class vars
    this->interface = interface;
    delete[] time_servers; // Delete if already allocated
    time_servers = new SntpServerInfo_t[num_ntp_servers];
    for(size_t time_server_idx = 0; time_server_idx < num_ntp_servers; time_server_idx++) {
        time_servers[time_server_idx].pServerName = ntp_servers[time_server_idx];
        time_servers[time_server_idx].serverNameLen = strlen(ntp_servers[time_server_idx]);
        time_servers[time_server_idx].port = SNTP_DEFAULT_SERVER_PORT;
    }

    // Make sure RTC is initialized
    RealTimeClock::init();

    // Create socket. Binding to any random local port is OK.
    socket.open(interface);
    socket.set_blocking(false);

    // Init SNTP
    const uint32_t response_timeout_ms = 500;
    return Sntp_Init(&sntp_context, time_servers, num_ntp_servers, response_timeout_ms, ntp_packet_buffer, sizeof(ntp_packet_buffer),
        &NTPClient::resolveDNS, &NTPClient::getRTCTime, &NTPClient::saveTimeOffset, &udp_interface, nullptr);
}