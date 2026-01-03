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
#include "arm_hal_random.h"

// Maximum diff between client and server time for the offset value from coreSNTP to be valid.
// Comments say that this is either 34 or 68 years (they disagree). For safety using
// 33 years here, converted to seconds
static constexpr std::chrono::seconds MAX_DIFF_FOR_TIME_OFFSET(1041000000);

#if MBED_CONF_NTP_CLIENT_PREFER_LP_TICKER && defined(DEVICE_LPTICKER)
static constexpr bool USING_LP_TICKER = true;
#else
static constexpr bool USING_LP_TICKER = false;
#endif

NTPClient::NTPClient():
#if MBED_CONF_NTP_CLIENT_PREFER_LP_TICKER && defined(DEVICE_LPTICKER)
us_clock(get_lp_ticker_data())
#else
us_clock(get_us_ticker_data())
#endif
{
}

bool NTPClient::resolveDNS(const SntpServerInfo_t *pServerAddr, uint32_t *pIpV4Addr) {
    SocketAddress result;

    if(instance().interface->gethostbyname(pServerAddr->pServerName, &result, NSAPI_IPv4) != NSAPI_ERROR_OK) {
        return false;
    }

    memcpy(pIpV4Addr, result.get_ip_bytes(), sizeof(uint32_t));
    return true;
}

void NTPClient::getCurrentTime(SntpTimestamp_t *pCurrentTime) {
    // Set seconds based on real-time clock. We need to convert from NTP epoch (Jan 1 1900) to
    // UNIX time (epoch = Jan 1 1970)
    const auto time_since_1970 = instance().now().time_since_epoch();
    const auto secs_since_1970 = std::chrono::floor<std::chrono::seconds>(time_since_1970);
    const auto fractional_seconds = std::chrono::microseconds(time_since_1970 % secs_since_1970);
    pCurrentTime->seconds = secs_since_1970.count() + SNTP_TIME_AT_UNIX_EPOCH_SECS;
    pCurrentTime->fractions = fractional_seconds.count() * SNTP_FRACTION_VALUE_PER_MICROSECOND;
}

void NTPClient::saveTimeOffset(const SntpServerInfo_t *pTimeServer, const SntpTimestamp_t *pServerTime,
    int64_t clockOffsetMs, SntpLeapSecondInfo_t leapSecondInfo) {
    // Currently we don't use the time server addr or leap second info.
    (void)pTimeServer;
    (void)leapSecondInfo;

    // Per the coreSNTP docs, if the client and server are more than 68 years apart,
    // clockOffsetMs is not valid. Check if that is the case and, if so, just set our clock
    // to the server time to get us in the right ballpark.
    time_point server_time(std::chrono::seconds(pServerTime->seconds - SNTP_FRACTION_VALUE_PER_MICROSECOND) +
        std::chrono::microseconds(pServerTime->fractions / SNTP_FRACTION_VALUE_PER_MICROSECOND));
    if (std::chrono::abs(instance().now() - server_time) > MAX_DIFF_FOR_TIME_OFFSET) {
        // To "set" the time, subtract away the current value of time_offset and replace it with the
        // value that would make the time equal server_time
        instance().most_recent_correction = server_time - instance().us_clock.now() - instance().time_offset;
    }
    else {
        instance().most_recent_correction = std::chrono::milliseconds(clockOffsetMs);
    }

    instance().time_offset += instance().most_recent_correction;

#if DEVICE_RTC
    // Write the current UNIX timestamp into the RTC as well.
    // We need to do some casting in order to round to seconds, then convert to an RTC time point.
    RealTimeClock::write(RealTimeClock::time_point(std::chrono::round<std::chrono::seconds>(instance().now().time_since_epoch())));
#endif
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
        // Return 0 to indicate no data avail within timeout
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

SntpStatus_t NTPClient::init(NetworkInterface *interface, std::chrono::milliseconds timeout, char const * const *ntp_servers, size_t num_ntp_servers) {

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

    if (!USING_LP_TICKER) {
        // Prevent deep sleep because the us ticker will turn off in deep sleep, so we will lose the time
        sleep_manager_lock_deep_sleep();
    }

#if DEVICE_RTC
    // Make sure RTC is initialized
    RealTimeClock::init();

    // If we have RTC time, set our initial time from it.
    // Example: if RTC is reading 10 seconds and us_clock is reading 1 second, then we want to set
    // time_offset to +9 seconds so that now() will return 10 seconds
    time_offset = RealTimeClock::now().time_since_epoch() - us_clock.now().time_since_epoch();
#endif

    // Create socket. Binding to any random local port is OK.
    // Note that the coreSNTP docs say that a nonblocking socket is recommended, but if we use a nonblocking socket,
    // then Sntp_ReceiveTimeResponse() will spinlock for the entire timeout, which is gross.
    socket.open(interface);
    socket.set_timeout(timeout.count());

    // Init SNTP
    return Sntp_Init(&sntp_context, time_servers, num_ntp_servers, timeout.count(), ntp_packet_buffer, sizeof(ntp_packet_buffer),
        &NTPClient::resolveDNS, &NTPClient::getCurrentTime, &NTPClient::saveTimeOffset, &udp_interface, nullptr);
}

NTPClient::time_point NTPClient::now() const {
    return us_clock.now() + time_offset;
}

SntpStatus_t NTPClient::requestTime() {

    // Prepare random number
    if(!randGen.has_value()) {
        // If we have TRNG support, seed the random number generator using it.
        // Otherwise, use the most accurate boot time timestamp available, which is... better than nothing as
        // the time between boot and now often isn't consistent at the microsecond level
#if defined(DEVICE_TRNG) || defined(FEATURE_PSA)
        randGen.emplace(arm_random_seed_get());
#else
        randGen.emplace(us_ticker_read());
#endif
    }
    const uint32_t randNumber = std::uniform_int_distribution<uint32_t>()(*randGen);

    // Note: I'm not aware of any reason why we would not be able to send the packet immediately that isn't
    // "permanent", e.g. the network being down. So, leaving the last parameter at 0 to disable retries.
    return Sntp_SendTimeRequest(&sntp_context, randNumber, 0);
}

SntpStatus_t NTPClient::receiveTime(TimeOffset &result) {

    // This will call saveTimeOffset() if a valid packet was received
    const auto ret = Sntp_ReceiveTimeResponse(&sntp_context, sntp_context.responseTimeoutMs);

    if(ret == SntpSuccess) {
        // Time offset was delivered through the callback
        result = most_recent_correction;
    }
    return ret;
}
