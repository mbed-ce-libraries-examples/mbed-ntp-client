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

#include "mbed.h"

#include "core_sntp_client.h"

#include <random>
#include <optional>

/**
 * @brief NTP client for Mbed OS.
 *
 * This library is a wrapper around FreeRTOS coreSNTP that connects it to the Mbed OS APIs for time and networking.
 *
 * \par NTP and Mbed Timers
 * Mbed OS has three different timer APIs that are able to be used by the NTP client. The us ticker is supported
 * on all Mbed devices, and provides a 64-bit microseconds timestamp. The real-time clock (RTC) is only available
 * on some Mbed devices, and only has seconds precision, but is able to preserve its time across resets
 * (which other timers cannot). Finally, the low precision (lp) ticker is similar to the us ticker but
 * uses a slower clock that is more power-efficient but less precise (its resolution is usually in the 10s-100s of
 * microseconds). Note that all three of these timers may or may not be clocked from the same clock source,
 * so it's possible for them to drift relative to each other over time on some targets.
 *
 * \par
 * In the base configuration, \c NTPClient produces a UNIX timestamp based on a microsecond ticker (either the us
 * or lp ticker). This UNIX timestamp gives the number of microseconds that have elapsed since January 1, 1970.
 * Internally, this is implemented as a fixed offset to the time returned by the microsecond ticker. This
 * offset is updated each time the time is synchronized.
 *
 * \par RTC Usage
 * If the RTC is available on the current target, each time the time is synchronized, the RTC is set to the seconds
 * portion of the current UNIX timestamp. Then, on init, the RTC is used to initialize the UNIX time to local
 * time offset before the first time sync is performed. This means that time can be preserved across resets
 * (though only with seconds precision, not microseconds!). If a backup battery is used to power the RTC in your
 * design, then the time can be kept across power losses as well.
 *
 * \par LP Ticker vs US Ticker
 * By default, if the lp ticker is supported on your target, it will be used. This is because while the lp
 * ticker has lower precision, it still operates in deep sleep mode. The us ticker does not run in deep sleep
 * mode, so the NTP client has to lock the deep sleep so that we don't lose track of the time.
 * If you prefer higher precision at the cost of not being able to deep sleep, you may set the
 * \c `ntp-client.prefer-lp-ticker` option to false in mbed_app.json5 to disable this behavior.
 * Also note that the underlying coreSNTP uses milliseconds, not microseconds, so the accuracy
 * of the NTP time can never be better than 1ms regardless of the underlying clock.
 *
 * @note This class is a singleton (because coreSNTP relies on global callbacks, so it's impossible to create
 * multiple instances) but still needs to be initialized before use by calling \c init().
 */
class NTPClient {

public:
    // Typedefs/definitions to make NTPClient an std::chrono clock
    using duration = TickerDataClock::duration;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = TickerDataClock::time_point;
    static const bool is_steady = false;

private:

    // coreSNTP data
    SntpContext_t sntp_context = {};
    SntpServerInfo_t * time_servers = nullptr;

    // Buffer for the library to build packets.
    uint8_t ntp_packet_buffer[SNTP_PACKET_BASE_SIZE];

    // Network interface to use
    NetworkInterface *interface;

    // Socket
    UDPSocket socket;

    // Internal microsecond-precision clock
    TickerDataClock us_clock;

    // Current best offset between the raw time returned by us_clock and the UNIX timestamp
    duration time_offset;

    // Most recent correct that we made to time_offset
    duration most_recent_correction;

    // Random number generator, created on first use
    std::optional<std::minstd_rand> randGen;

    // Private constructor
    NTPClient();

    // DNS resolve callback. See SntpResolveDns_t for docs.
    static bool resolveDNS(const SntpServerInfo_t * pServerAddr, uint32_t * pIpV4Addr);

    // Get time callback. See SntpGetTime_t for docs.
    static void getCurrentTime(SntpTimestamp_t * pCurrentTime);

    // Time sync performed callback. See SntpSetTime_t for details.
    static void saveTimeOffset(const SntpServerInfo_t * pTimeServer, const SntpTimestamp_t * pServerTime, int64_t clockOffsetMs, SntpLeapSecondInfo_t leapSecondInfo);

    // Send UDP bytes to a network address. See UdpTransportSendTo_t for docs
    static int32_t udpSendto(NetworkContext_t * pNetworkContext, uint32_t serverAddr, uint16_t serverPort, const void * pBuffer, uint16_t bytesToSend);

    // Receive UDP bytes from a network address. See UdpTransportRecvFrom_t for docs
    static int32_t udpRecvfrom(NetworkContext_t * pNetworkContext, uint32_t serverAddr, uint16_t serverPort, void * pBuffer, uint16_t bytesToRecv);

    // Interface to coreSNTP for UDP functions
    static constexpr UdpTransportInterface_t udp_interface{
        .pUserContext = nullptr, // not used
        .sendTo = &udpSendto,
        .recvFrom = &udpRecvfrom
    };

public:
    /**
     * @brief Get instance of NTPClient
     */
    static NTPClient & instance();

    static constexpr char const * DEFAULT_NTP_SERVERS[] = {"2.pool.ntp.org"};

    static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT = 1s;

    /**
     * @brief Initialize Mbed NTP.
     *
     * @param interface Network interface to communicate over
     * @param timeout Timeout to wait for a response from the server in receiveTime(). May be 0 to operate in nonblocking mode.
     * @param ntp_servers Array of NTP server addresses, as C strings. Optional, defaults to DEFAULT_NTP_SERVERS.
     *     Strings must remain allocated as long as the class is used.
     * @param num_ntp_servers Number of NTP servers in \c ntp_servers
     *
     * @return Error code or \c SntpSuccess
     */
    SntpStatus_t init(NetworkInterface *interface,
                      std::chrono::milliseconds timeout = DEFAULT_TIMEOUT,
                      char const * const * ntp_servers = DEFAULT_NTP_SERVERS,
                      size_t num_ntp_servers = sizeof(DEFAULT_NTP_SERVERS) / sizeof(char *));

    /**
     * @return The current time, as a UNIX timestamp (microseconds since 1970).
     *
     * @warning This time is NOT monotonic and can jump backwards or forwards depending on NTP
     *    synchronization status. Also note that this can be called before the class is initialized,
     *    but may jump backward when \c init() is called.
     */
    time_point now() const;

    /**
     * @brief Manually set the offset between the internal high precision clock and NTP time.
     *
     * This replaces the offset that was previously obtained from NTP or the real-time clock.
     *
     * @warning This is not thread-safe if other threads may potentially be reading the NTP time
     *    (the other threads might briefly see a garbage time value).
     *    It is mainly intended for testing purposes.
     */
    void set_offset(const duration offset) {
        time_offset = offset;
    }

    /**
     * @brief Send a packet to the server requesting the time.
     *
     * The specific NTP server to use is determined by the underlying coreSNTP library. Generally, the first
     * NTP server passed will be used, unless that server did not respond to or actively rejected a previous
     * query, in which case the next server down the list will be used.
     *
     * This function only sends the NTP query, it does not process the response or synchronize the time.
     * Call \c receiveTime() at a later date to process the result of the query.
     *
     * @return Error code or \c SntpSuccess
     */
    SntpStatus_t requestTime();

    /**
     * @brief Receive the result of a previous time query to the NTP server and update the time.
     *
     * You must previously have called \c requestTime() to initiate an NTP query.
     *
     * @note If no response is received (or the server rejects the request), this function does not make a new
     *     time request, but does rotate to the next time server in the list to use the next time \c requestTime()
     *     is called.
     *
     * @param[out] offset If successful, the time offset that was compensated is saved here. For example,
     *    if this is 1ms, it means our local time was 1ms behind the server's, so the clock was advanced
     *    by 1ms.
     *
     * @return Error code or \c SntpSuccess
     * @retval SntpErrorResponseTimeout if no response was received from the NTP server within the configured timeout
     */
    SntpStatus_t receiveTime(std::chrono::microseconds & offset);

    /**
     * @brief Attempt a blocking sync of the Mbed system time in the RTC.
     *
     * @param[out] offset If successful, the time offset that was compensated is saved here. For example,
     *    if this is 1ms, it means our local time was 1ms behind the server's, so the clock was advanced
     *    by 1ms.
     *
     * @return SntpSuccess on success, or error code on error
     * @retval SntpErrorResponseTimeout if no response was received from the NTP server within the configured timeout
     *
     * @note Only one NTP request is sent by this function (no retries). However, if no response or an explicit rejection
     *     is received from the server, this call does rotate to the next time server in the list to use next time.
     *     So, you may wish to call this function multiple times, especially if you have multiple time servers configured.
     */
    SntpStatus_t syncSystemTime(std::chrono::microseconds & offset) {
        auto ret = requestTime();
        if (ret != SntpSuccess) { return ret; }
        return receiveTime(offset);
    }
};
