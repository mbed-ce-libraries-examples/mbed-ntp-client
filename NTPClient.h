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

/**
 * @brief NTP client for Mbed OS.
 *
 * This library is a wrapper around FreeRTOS coreSNTP that connects it to the Mbed OS APIs for time and networking.
 *
 * Note: This class is a singleton (because coreSNTP relies on global callbacks, so it's impossible to create
 * multiple instances) but still needs to be initialized before use by calling \c init().
 */
class NTPClient {

public:
    /**
     * @brief Structure representing a time offset obtained by NTP.
     *
     * This structure describes how the system time relates to the NTP time, in the form
     * system_time + offset = ntp_time. So if the offset is 1 second and 1 millisecond, then we are
     * 1.001 seconds behind the NTP server time.
     */
    struct TimeOffset {
        /// Total time offset in milliseconds
        std::chrono::milliseconds offset;

        /// Get the sub-second part of the offset
        std::chrono::milliseconds subsecondPart() {
            return offset % std::chrono::seconds(1);
        }

        /// Get the whole seconds part of the offset
        std::chrono::seconds wholeSeconds() {
            return std::chrono::floor<std::chrono::seconds>(offset);
        }
    };

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

    // Most recent time offset obtained from the NTP server
    TimeOffset last_time_offset;

    // Private constructor
    NTPClient() = default;

    // DNS resolve callback. See SntpResolveDns_t for docs.
    static bool resolveDNS(const SntpServerInfo_t * pServerAddr, uint32_t * pIpV4Addr);

    // Get time callback. See SntpGetTime_t for docs.
    static void getRTCTime(SntpTimestamp_t * pCurrentTime);

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

    /**
     * @brief Initialize Mbed NTP.
     *
     * @param interface Network interface to communicate over
     * @param ntp_servers Array of NTP server addresses, as C strings. Optional, defaults to DEFAULT_NTP_SERVERS.
     *     Strings must remain allocated as long as the class is used.
     * @param num_ntp_servers Number of NTP servers in \c ntp_servers
     *
     * @return Error code or \c SntpSuccess
     */
    SntpStatus_t init(NetworkInterface *interface,
                      char const * const * ntp_servers = DEFAULT_NTP_SERVERS,
                      size_t num_ntp_servers = sizeof(DEFAULT_NTP_SERVERS) / sizeof(char *));

};
