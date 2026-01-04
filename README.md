# Mbed OS NTP Client

This library allows you to fetch time information from a NTP server.

It has been completely rewritten by the Mbed CE project, and now has the following features:

- Better compliance with the (S)NTP standard thanks to using FreeRTOS coreSNTP under the hood
- Support for multiple NTP servers with automatic failover
- Sub-second time synchronization (thanks to using the Mbed us ticker as a high resolution clock)
- Automatically manages the RTC for you to keep time across resets, if supported on your hardware
- Can be used in two-part sync mode (first send, then receive later) so that it can be used in a non-blocking way from e.g. a background polling function

## Basic API

### `NTPClient::instance()`

Get the singleton instance of the NTP client. All other functions must be called on this instance.

### `NTPClient::init(NetworkInterface *iface)` [init function]

Init the NTP client. You need to provide a pointer to an [Mbed OS NetworkInterface](https://os.mbed.com/docs/mbed-os/v6.15/apis/network-socket.html). The interface does not need to be up when this is called, but should be up 

### `NTPClient::time_point NTPClient::now()`

Get the current time from the NTP clock, as microseconds since 1970. This uses the [std::chrono::time_point](https://en.cppreference.com/w/cpp/chrono/time_point.html) class.

### `SntpStatus_t NTPClient::syncTime()`

Synchronize the NTP time with the currently selected NTP server.
