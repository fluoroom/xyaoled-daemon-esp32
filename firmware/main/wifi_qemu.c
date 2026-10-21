// The wifi.h API over QEMU's emulated Ethernet (OpenCores MAC), for the
// QEMU build (CONFIG_XY_QEMU): the network comes up through DHCP from
// QEMU's user networking; there is no radio, so no scan and no portal.
#include <string.h>

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "net_qemu";

static wifi_status_t st;

const char *wifi_state_name(wifi_state_t s)
{
	static const char *names[] = {"idle", "connecting", "connected", "failed"};
	return names[s];
}

void wifi_set_ap_hook(void (*hook)(bool))
{
}

void wifi_status(wifi_status_t *s)
{
	*s = st;
}

cJSON *wifi_scan(void)
{
	return cJSON_CreateArray();
}

esp_err_t wifi_try(const char *ssid, const char *password)
{
	return ESP_ERR_NOT_SUPPORTED;
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	ip_event_got_ip_t *e = data;
	snprintf(st.ip, sizeof st.ip, IPSTR, IP2STR(&e->ip_info.ip));
	st.state = WIFI_CONNECTED;
	ESP_LOGI(TAG, "Ethernet up, IP %s", st.ip);
	const settings_t *s = settings_get();
	if (mdns_init() == ESP_OK) {
		mdns_hostname_set(s->hostname);
		mdns_service_add(NULL, "_http", "_tcp", s->http_port, NULL, 0);
	}
}

esp_err_t wifi_start(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
	esp_netif_t *netif = esp_netif_new(&ncfg);

	eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
	eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
	phy_cfg.autonego_timeout_ms = 100;
	esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
	esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
	esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
	esp_eth_handle_t eth;
	ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
	ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
	esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL);
	strcpy(st.ssid, "qemu-ethernet");
	st.state = WIFI_CONNECTING;
	return esp_eth_start(eth);
}
