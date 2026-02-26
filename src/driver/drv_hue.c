#include <time.h>

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
// Commands register, execution API and cmd tokenizer
#include "../cmnds/cmd_public.h"
#include "../mqtt/new_mqtt.h"
#include "../logging/logging.h"
#include "../hal/hal_pins.h"
#include "../hal/hal_wifi.h"
#include "drv_public.h"
#include "drv_local.h"
#include "drv_ssdp.h"
#include "../httpserver/new_http.h"
#include "../cJSON/cJSON.h"

// Hue driver for Alexa, requiring btsimonh's SSDP to work
// Based on Tasmota approach
// The procedure is following:
// 1. first MSEARCH over UDP is done
// 2. then obk replies to MSEARCH with page details
// 3. then alexa accesses our XML pages here with GET
// 4. and can change the binary state (0 or 1) with POST

#define MAX_HUE_DEVICES 32
#define ECHO_GEN 2

static char *buffer_out = 0;
static char *g_serial = 0;
static char *g_userID = 0;
static char *g_uid = 0;
static char *g_bridgeID = 0;
static int outBufferLen = 0;
static int stat_searchesReceived = 0;
static int stat_setupXMLVisits = 0;
// stubs
static int stat_metaServiceXMLVisits = 0;
static int stat_eventsReceived = 0;
static int stat_eventServiceXMLVisits = 0;


// ARGUMENTS: first IP, then bridgeID
const  char *hue_resp = "HTTP/1.1 200 OK\r\n"
   "HOST: 239.255.255.250:1900\r\n"
   "CACHE-CONTROL: max-age=100\r\n"
   "EXT:\r\n"
   "LOCATION: http://%s:80/description.xml\r\n"
   "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.24.0\r\n"  // was 1.17
   "hue-bridgeid: %s\r\n";

// ARGUMENTS: uuid
const  char *hue_resp1 = "ST: upnp:rootdevice\r\n"
  "USN: uuid:%s::upnp:rootdevice\r\n"
  "\r\n";

// ARGUMENTS: uuid and uuid
const  char *hue_resp2 =  "ST: uuid:%s\r\n"
   "USN: uuid:%s\r\n"
   "\r\n";

// ARGUMENTS: uuid
const  char *hue_resp3 = "ST: urn:schemas-upnp-org:device:basic:1\r\n"
   "USN: uuid:%s\r\n"
   "\r\n";

void DRV_HUE_Send_Advert_To(struct sockaddr_in *addr) {
	//const char *useType;

	if (g_uid == 0) {
		// not running
		return;
	}

	stat_searchesReceived++;

	if (buffer_out == 0) {
		outBufferLen = strlen(hue_resp) + 256;
		buffer_out = (char*)malloc(outBufferLen);
	}
	{
		// ARGUMENTS: first IP, then bridgeID
		snprintf(buffer_out, outBufferLen, hue_resp, HAL_GetMyIPString(), g_bridgeID);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[0] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid
		snprintf(buffer_out, outBufferLen, hue_resp1, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[1] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid and uuid
		snprintf(buffer_out, outBufferLen, hue_resp2, g_uid, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[2] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
	{
		// ARGUMENTS: uuid
		snprintf(buffer_out, outBufferLen, hue_resp3, g_uid);

		addLogAdv(LOG_ALL, LOG_FEATURE_HTTP, "HUE - Sending[3] %s", buffer_out);
		DRV_SSDP_SendReply(addr, buffer_out);
	}
}


void HUE_AppendInformationToHTTPIndexPage(http_request_t* request, int bPreState) {
	if(bPreState)
		return;
	hprintf255(request, "<h4>HUE: searches %i, setup %i, events %i, mService %i, event %i </h4>",
		stat_searchesReceived, stat_setupXMLVisits, stat_eventsReceived, stat_metaServiceXMLVisits, stat_eventServiceXMLVisits);
	
	ADDLOG_INFO("DRV_HUE - HUE_Setup registered, %s", NULL);
}

const char *g_hue_setup_1 = "<?xml version=\"1.0\"?>"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
"<specVersion>"
"<major>1</major>"
"<minor>0</minor>"
"</specVersion>"
"<URLBase>http://";
// IP ADDR HERE
const char *g_hue_setup_2 = ":80/</URLBase>"
"<device>"
"<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>"
"<friendlyName>Amazon-Echo-HA-Bridge (";
// IP ADDR HERE
const char *g_hue_setup_3 = ")</friendlyName>"
"<manufacturer>Royal Philips Electronics</manufacturer>"
"<manufacturerURL>http://www.philips.com</manufacturerURL>"
"<modelDescription>Philips hue Personal Wireless Lighting</modelDescription>"
"<modelName>Philips hue bridge 2012</modelName>"
"<modelNumber>929000226503</modelNumber>"
"<serialNumber>";
// SERIAL here
const char *g_hue_setup_4 = "</serialNumber>"
"<UDN>uuid:";
// UID HERE
const char *g_hue_setup_5 = "</UDN>"
   "</device>"
   "</root>\r\n"
   "\r\n";

static int HUE_Setup(http_request_t* request) {
	http_setup(request, httpMimeTypeXML);
	poststr(request, g_hue_setup_1);
	poststr(request, HAL_GetMyIPString());
	poststr(request, g_hue_setup_2);
	poststr(request, HAL_GetMyIPString());
	poststr(request, g_hue_setup_3);
	poststr(request, g_serial);
	poststr(request, g_hue_setup_4);
	poststr(request, g_uid);
	poststr(request, g_hue_setup_5);
	poststr(request, NULL);

	stat_setupXMLVisits++;

	return 0;
}

static char* HUE_DeviceId(int id) {
	char s[32];
	unsigned char mac[8];
	WiFI_GetMacAddress((char*)mac);
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02X-01", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (id >> 8) & 0xFF, (id & 0xFF) ^ 0x10);

	return s;
}


static int HUE_NotImplemented(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{}");
	poststr(request, NULL);

	return 0;
}
static int HUE_Authentication(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "[{\"success\":{\"username\":\"%s\"}}]",g_userID);
	poststr(request, NULL);

	return 0;
}
static int HUE_Config_Internal(http_request_t* request, bool gconfig) {

	unsigned char mac[8];
	WiFI_GetMacAddress((char*)mac);
	char mac_str[18];
	snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	unsigned int date_time = NTP_GetCurrentTime();
	struct tm *utc_tm = gmtime((time_t *) &date_time);
	char utc_time_str[32];
	strftime(utc_time_str, sizeof(utc_time_str), "%Y-%m-%dT%H:%M:%S", utc_tm);

	if (!gconfig) http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"name\":\"Philips hue\",\"mac\":\"");
	// Mac address
	poststr(request, mac_str);
	poststr(request, "\",\"dhcp\":true,\"ipaddress\":\"");
	// IP address
	poststr(request, HAL_GetMyIPString());
	poststr(request, "\",\"netmask\":\"");
	// Netmask
	poststr(request, HAL_GetMyMaskString());
	poststr(request, "\",\"gateway\":\"");
	// Gateway
	poststr(request, HAL_GetMyGatewayString());
	poststr(request, "\",\"proxyaddress\":\"none\",\"proxyport\":0,\"bridgeid\":\"");
	// BridgeID
	poststr(request, g_bridgeID);
	poststr(request, "\",\"UTC\":\"");
	// UTC time
	poststr(request, utc_time_str);
	poststr(request, "\",\"whitelist\":{\"");
	// UserID
	poststr(request, g_userID);
	poststr(request, "\":{\"last use date\":\"");
	// UTC time
	poststr(request, utc_time_str);
	poststr(request, "\",\"create date\":\"");
	// UTC time
	poststr(request, utc_time_str);
	poststr(request, "\",\"name\":\"Remote\"}},\"swversion\":\"01041302\",\"apiversion\":\"1.17.0\",\"swupdate\":{\"updatestate\":0,\"url\":\"\",\"text\":\"\",\"notify\": false},\"linkbutton\":false,\"portalservices\":false}");
	poststr(request, NULL);

	return 0;
}

bool endsWith(const char *str, const char *suffix) {
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr) return false;
	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

static int HUE_CountDevices() {
	// Returns the number of devices in use
	int nb_channels = 0;
	for (int i = 1; i < CHANNEL_MAX; i++) {
		if (CHANNEL_IsInUse(i)) nb_channels++;
	}
	return nb_channels;
}

static bool HUE_IsActive(int device) {
	// Check whether this device should be reported to Alexa or considered hidden.
	// Any device whose friendly name start with "$" is considered hidden
	// TODO: implement this
	return true;
}

static int HUE_LightStatus1(int device, http_request_t* request) {

	// TODO: implement lights
	char bri = 254;
	poststr(request, "{\"on\":");
	poststr(request, (CHANNEL_Get(device)) ? "true" : "false");
	poststr(request, ",\"alert\":\"none\",\"effect\":\"none\",\"reachable\":true}");

	return 0;
}

static int HUE_LightStatus2(int device, http_request_t* request) {
	poststr(request, ",\"type\":\"Extended color light\",\"name\":\"");
	poststr(request, CFG_GetShortDeviceName());
	poststr(request, "\",\"modelid\":\"");
	poststr(request, CFG_GetDeviceName());
	poststr(request, "\",\"manufacturername\":\"OpenBeken\",\"uniqueid\":\"");
	poststr(request, HUE_DeviceId(device));
	poststr(request, "\"}");

	return 0;
}

static int HUE_EncodeLightId(int device) {
	unsigned char mac[8];
	WiFI_GetMacAddress((char*)mac);
	int id = (mac[3] << 20) | (mac[4] << 12) | (mac[5] << 4);

	if (device >= 32) device = 0;
	if (device > 15) {
		id |= (1 << 28);
	}
	id |= (device & 0xF);

	return id;
}

static char HUE_DecodeLightId(int hue_id) {
	char device = hue_id & 0xF;
	if (hue_id & (1 << 28)) {   // check if bit 25 is set, if so we have
		device += 16;
	}
	if (0 == device) {        // special value 0 is actually relay #32
		device = 32;
	}
	return device;
}

static int HUE_CheckCompatible(http_request_t* request, bool appending) {
	int nb_devices = HUE_CountDevices();
	int maxhue = (nb_devices > MAX_HUE_DEVICES) ? MAX_HUE_DEVICES : nb_devices;
	char device_id_str[12];
	for (int i = 1; i <= maxhue; i++) {
		if (HUE_IsActive(i)) {
			if (appending) { poststr(request, ","); }
			poststr(request, "\"");
			sprintf(device_id_str, "%d", HUE_EncodeLightId(i));
			poststr(request, device_id_str);
			poststr(request, "\":{\"state\":");
			HUE_LightStatus1(i, request);
			HUE_LightStatus2(i, request);
			appending = true;
		}
	}

	return 0;
}

static int HUE_LightsCommand(int device, int device_id, http_request_t* request) {
	// TODO: implement lights commands
	char device_id_str[12];
	sprintf(device_id_str, "%d", device_id);
	http_setup(request, httpMimeTypeJson);
	int len = request->bodylen;
	char json_str[len+1];
	memcpy(json_str, request->bodystart, len);
	json_str[len] = '\0';
	const cJSON *json = cJSON_Parse(json_str);
	if (json) {
		const cJSON *on = cJSON_GetObjectItem(json, "on");
		CHANNEL_Set(device, cJSON_IsTrue(on), 0);
	}
	poststr(request, "[{\"success\":{\"/lights/");
	poststr(request, device_id_str);
	poststr(request, "/state/on\":");
	poststr(request, CHANNEL_Get(device) ? "true" : "false");
	poststr(request, "}}]");
	poststr(request, NULL);
}

static int HUE_Lights(http_request_t* request) {
	// TODO: lights
	int device = 1;
	char device_id_str[12];
	int device_id;   // the raw device_id used by Hue emulation
	int nb_devices = HUE_CountDevices();
	int maxhue = (nb_devices > MAX_HUE_DEVICES) ? MAX_HUE_DEVICES : nb_devices;
	const char *command = strstr(request->url, "/lights");

	if (endsWith(command, "/lights")) {
		bool appending = false;
		http_setup(request, httpMimeTypeJson);
		poststr(request, "{");
		HUE_CheckCompatible(request, appending);
		poststr(request, "}");
		poststr(request, NULL);
	} else if (endsWith(command, "/state")) {
		
		char *id_begin = command + 8; // skip "/lights/"
		char *id_end = strstr(command, "/state");
		int len_id = id_end - id_begin;
		strncpy(device_id_str, id_begin, len_id);
		device_id = atoi(device_id_str);
		device = HUE_DecodeLightId(device_id);

		if ((device >= 1) || (device <= maxhue)) {
			HUE_LightsCommand(device, device_id, request);
		}
	} else if (strstr(command, "/lights/")) {
		command += 8; // skip "/lights/"
		device_id = atoi(command);
		device = HUE_DecodeLightId(device_id);
		if ((device >= 1) && (device <= maxhue)) {
			device = 1;
		}
		http_setup(request, httpMimeTypeJson);
		poststr(request, "{\"state\":");
		HUE_LightStatus1(device, request);
		HUE_LightStatus2(device, request);
		poststr(request, NULL);
	} else {
		http_setup(request, httpMimeTypeJson);
		poststr(request, "{}");
		poststr(request, NULL);
	}
	return 0;
}

static int HUE_Groups(http_request_t* request) {
	// TODO: groups
	ADDLOG_INFO(LOG_FEATURE_HTTP, "HUE - Groups not implemented");
	return HUE_NotImplemented(request);
}

static int HUE_GlobalConfig(http_request_t* request) {

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"lights\":{");
	// TODO: lights
	poststr(request, "},\"groups\":{},\"schedules\":{},\"config\":");
	HUE_Config_Internal(request, true);
	poststr(request, "}");
	poststr(request, NULL);

	return 0;
}

// http://192.168.0.213/api/username/lights/1/state
// http://192.168.0.213/description.xml
int HUE_APICall(http_request_t* request) {
	if (g_uid == 0) {
		// not running
		return 0;
	}
	if (strncmp(request->url, "api", 3) != 0 && (request->url[3] != '/' || request->url[3] != '\0')) {
		// not for HUE
		return 0;
	}
	// skip "api"
	const char *api = request->url + 3;
	if (*api == '\0' || strcmp(api, "/") == 0) {
		// Handled by HUE
		HUE_Authentication(request);
		return 1;
	}
	const char *checkhere[] = {"/channels", "/pins", "/channelTypes", "/logconfig", "/reboot", 
							   "/flash", "/info", "/dumpconfig", "/testconfig", "/testflashvars", 
							   "/seriallog", "/lfs", "/run", "/del", "/cmnd", "/ota", "/fsblock"};
	for (int i = 0; i < sizeof(checkhere) / sizeof(checkhere[0]); i++) {
		if (strncmp(api, checkhere[i], strlen(checkhere[i])) == 0) {
			// not for HUE
			return 0;
		}
	}

	char buf[128];
	int len = request->bodylen;
	if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
	memcpy(buf, request->bodystart, len);
	buf[len] = 0;
	ADDLOG_INFO(LOG_FEATURE_HTTP, "HUE - body: %s", buf);

	if (endsWith(api, "/config")) HUE_Config_Internal(request, false);
	else if (strstr(api, "/lights")) HUE_Lights(request);
	else if (strstr(api, "/groups")) HUE_Groups(request);
	else if (endsWith(api, "/schedules")) HUE_NotImplemented(request);
	else if (endsWith(api, "/sensors")) HUE_NotImplemented(request);
	else if (endsWith(api, "/scenes")) HUE_NotImplemented(request);
	else if (endsWith(api, "/rules")) HUE_NotImplemented(request);
	else if (endsWith(api, "/resourcelinks")) HUE_NotImplemented(request);
	else HUE_GlobalConfig(request);

	return 1;
}
// backlog startDriver SSDP; startDriver HUE
// 
void HUE_Init() {
	char tmp[64];
	unsigned char mac[8];

	WiFI_GetMacAddress((char*)mac);
	// username - 
	snprintf(tmp, sizeof(tmp), "%02x%02x%02x",  mac[3], mac[4], mac[5]);
	g_userID = strdup(tmp);
	// SERIAL - as in Tas, full 12 chars of MAC, so 5c cf 7f 13 9f 3d
	snprintf(tmp, sizeof(tmp), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	g_serial = strdup(tmp);
	// BridgeID - as in Tas, full 12 chars of MAC with FFFE inside, so 5C CF 7F FFFE 13 9F 3D
	snprintf(tmp, sizeof(tmp), "%02X%02X%02XFFFE%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	g_bridgeID = strdup(tmp);
	// uuid
	snprintf(tmp, sizeof(tmp), "f6543a06-da50-11ba-8d8f-%s", g_serial);
	g_uid = strdup(tmp);

	ADDLOG_INFO(LOG_FEATURE_HTTP, "HUE init - Serial %s, BridgeID %s, UID %s, username %s", g_serial, g_bridgeID, g_uid, g_userID);

	//HTTP_RegisterCallback("/api", HTTP_ANY, HUE_APICall);
	HTTP_RegisterCallback("/description.xml", HTTP_GET, HUE_Setup, 0);
}
