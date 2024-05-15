#include <guiconfig/guiconfig.h>

#include "MItem_network.hpp"
#include <espif.h>
#include <dns.h>
#include "wui_api.h"
#include "netdev.h"
#include "ScreenHandler.hpp"
#include "marlin_client.hpp"
#include <dialog_text_input.hpp>
#include <wui.h>

namespace {
bool is_device_connected(netdev_status_t status) {
    return status == NETDEV_NETIF_UP || status == NETDEV_NETIF_NOADDR;
}

void ip2str(const ip4_addr_t &addr, char *buf, size_t len) {
    if (!addr.addr) {
        strlcpy(buf, "-", len);
        return;
    }

    ip4addr_ntoa_r(&addr, buf, len);
}
} // namespace

inline uint32_t NetDeviceID::get() const {
    return value_ == active_id ? netdev_get_active_id() : value_;
}

bool NetDeviceID::is_connected() const {
    return is_device_connected(netdev_get_status(get()));
}

MI_WIFI_STATUS_t::MI_WIFI_STATUS_t()
    : WI_INFO_t(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no) {
    update();
}

void MI_WIFI_STATUS_t::update() {
    const netdev_status_t status = netdev_get_status(NETDEV_ESP_ID);
    if (is_device_connected(status)) {
        ChangeInformation(status == NETDEV_NETIF_UP ? _("Connected") : _("Link down"));
        return;
    }

    const char *state_str = []() -> const char * {
        switch (esp_link_state()) {

        case EspLinkState::Init:
            switch (esp_fw_state()) {

            case EspFwState::Flashing:
            case EspFwState::NoFirmware:
            case EspFwState::WrongVersion:
                return N_("Flash ESP");

            case EspFwState::Scanning:
                // Wifi module is scanning for Access points
                return N_("Scanning");

            case EspFwState::NoEsp:
                return N_("Gone");

            case EspFwState::Ok:
                return N_("Link down");

            case EspFwState::Unknown:
                return N_("Unknown");
            }

            return nullptr;

        case EspLinkState::NoAp:
            return N_("No AP");

        case EspLinkState::Up:
            return N_("Connected");

        case EspLinkState::Silent:
            return N_("ESP error");
        }

        return nullptr;
    }();
    ChangeInformation(_(state_str));
}

// ===================================================
// MI_WIFI_INIT
// ===================================================
MI_WIFI_INIT_t::MI_WIFI_INIT_t()
    : IWindowMenuItem(_(label), nullptr, is_enabled_t(!marlin_client::is_printing()), is_hidden_t::no) {
}

void MI_WIFI_INIT_t::click([[maybe_unused]] IWindowMenu &window_menu) {
    marlin_client::gcode("M997 S1 O");
}

// ===================================================
// MI_WIFI_SSID
// ===================================================
MI_WIFI_SSID::MI_WIFI_SSID()
    : WiInfoString(string_view_utf8::MakeRAM(config_store().wifi_ap_ssid.get_c_str()), 24, _(label)) {
}

// ===================================================
// MI_WIFI_SETUP
// ===================================================
MI_WIFI_SETUP::MI_WIFI_SETUP()
    : IWindowMenuItem(_(label)) {
}

void MI_WIFI_SETUP::click(IWindowMenu &) {
    marlin_client::gcode("M1703");
}

MI_NET_INTERFACE_t::MI_NET_INTERFACE_t()
    : WI_SWITCH_t(0, _(label), nullptr, is_enabled_t::yes, is_hidden_t::no, string_view_utf8::MakeCPUFLASH((const uint8_t *)str_eth), string_view_utf8::MakeCPUFLASH((const uint8_t *)str_wifi)) {
    if (netdev_get_active_id() == NETDEV_ESP_ID) {
        this->SetIndex(1);
    } else {
        this->SetIndex(0);
    }
}

void MI_NET_INTERFACE_t::OnChange([[maybe_unused]] size_t old_index) {
    netdev_set_active_id(this->index);
}

MI_HOSTNAME::MI_HOSTNAME(NetDeviceID device_id)
    : WiInfo<config_store_ns::lan_hostname_max_len + 1>(_(label), nullptr, is_enabled_t::yes,
#if defined(USE_ST7789) || defined(USE_MOCK_DISPLAY)
        is_hidden_t::dev
#elif defined(USE_ILI9488)
        is_hidden_t::no
#endif
        )
    , device_id(device_id) //
{
    update();
}

void MI_HOSTNAME::update() {
    if (device_id() == NETDEV_ESP_ID) {
        ChangeInformation(config_store().wifi_hostname.get_c_str());
    } else {
        ChangeInformation(config_store().lan_hostname.get_c_str());
    }
}

MI_NET_IP::MI_NET_IP(NetDeviceID device_id)
    : WI_SWITCH_t(0, string_view_utf8::MakeCPUFLASH(label), nullptr, is_enabled_t::yes, is_hidden_t::no, string_view_utf8::MakeCPUFLASH(str_DHCP), string_view_utf8::MakeCPUFLASH(str_static))
    , device_id(device_id) //
{
    index = netdev_get_ip_obtained_type(this->device_id());
}

void MI_NET_IP::OnChange([[maybe_unused]] size_t old_index) {
    const auto dev_id = device_id();
    if (index == NETDEV_STATIC) {
        netdev_set_static(dev_id);
    } else {
        netdev_set_dhcp(dev_id);
    }
}

MI_NET_IP_VER_t::MI_NET_IP_VER_t()
    : WI_SWITCH_t(0, _(label), nullptr, is_enabled_t::no, is_hidden_t::no, string_view_utf8::MakeCPUFLASH(str_v4), string_view_utf8::MakeCPUFLASH(str_v6)) {
    this->index = 0;
}

IMI_IP4_ADDR::IMI_IP4_ADDR(const char *label, NetDeviceID device_id, AddrType addr)
    : WiInfo(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no)
    , device_id(device_id)
    , addr(addr) {
    update();
}

void IMI_IP4_ADDR::update() {
    if (!device_id.is_connected()) {
        ChangeInformation(UNKNOWN_ADDR);
        return;
    }

    lan_t config = {};
    netdev_get_ipv4_addresses(device_id(), &config);

    std::array<char, ADDR_LEN> str;
    ip2str(config.*addr, str.data(), str.size());
    ChangeInformation(str.data());
}

MI_IP4_DNS1::MI_IP4_DNS1()
    : WiInfo(_(label)) {
    update();
}

void MI_IP4_DNS1::update() {
    std::array<char, ADDR_LEN> str;
    ip2str(*dns_getserver(0), str.data(), str.size());
    ChangeInformation(str.data());
}

MI_MAC_ADDR::MI_MAC_ADDR(NetDeviceID device_id)
    : WiInfo<MAC_LEN>(_(label), nullptr, is_enabled_t::yes, is_hidden_t::no)
    , device_id(device_id) {
    update();
}

void MI_MAC_ADDR::update() {
    mac_address_t mac;
    get_MAC_address(&mac, device_id());
    ChangeInformation(mac);
}
