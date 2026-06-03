#include <esp_http_server.h>
#include <esp_log.h>
#include "esp_timer.h"
#include <string>
#include <string.h>
#include "cJSON.h"

#include "IntexSWG.h"
#include "utils.h"
#include "RestServer.h"

/********************************* OTA *******************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include "esp_ota_ops.h"
#include "freertos/event_groups.h"
/********************************* OTA *******************************************/

using namespace std;

/********************************* OTA *******************************************/
// Embedded Files. To add or remove make changes is component.mk file as well. 
extern const uint8_t index_html_start[] asm("_binary_indexOTA_html_start");
extern const uint8_t index_html_end[]   asm("_binary_indexOTA_html_end");

int8_t flash_status = 0;
int8_t enableota = 0;

EventGroupHandle_t reboot_event_group;
const int REBOOT_BIT = BIT0;
/********************************* OTA *******************************************/

/* @brief tag used for ESP serial console messages */
static const char TAG[] = "api_rest";

/* @brief the HTTP server handle */
static httpd_handle_t server = NULL;

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size){
    size_t remaining = req->content_len;
    size_t offset = 0;

    if (remaining >= buffer_size) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer + offset, remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        offset += received;
        remaining -= received;
    }

    buffer[offset] = '\0';
    return ESP_OK;
}

static cJSON *parse_request_json(httpd_req_t *req, char *buffer, size_t buffer_size){
    notifyApiRequest();
    if (read_request_body(req, buffer, buffer_size) != ESP_OK) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    return root;
}

// HTTP GET General info request
static esp_err_t general_info_get_handler(httpd_req_t *req){

    notifyApiRequest();
    httpd_resp_set_type(req, "application/json");
    
    std::string displayDigits;
    displayDigits += getDisplayDigitFromCode(displayingDigit2);
    displayDigits += getDisplayDigitFromCode(displayingDigit1);
    if (displayingDigit1 == DISP_1_CLEAN_06P || displayingDigit1 == DISP_1_CLEAN_10P || displayingDigit1 == DISP_1_CLEAN_14P) {
        displayDigits += '.';
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    
    cJSON *display = cJSON_CreateObject();
    cJSON_AddStringToObject(display, "status", (!displayON || powerStatus == POWER_STATUS_OFF) ? "OFF" : "ON");    
    cJSON_AddNumberToObject(display, "brightness", displayIntensity);
    cJSON_AddStringToObject(display, "current_code", displayDigits.c_str());
    cJSON_AddItemToObject(data, "display", display);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "power", (powerStatus == POWER_STATUS_BOOTING) ? "BOOTING" : (powerStatus == POWER_STATUS_ON) ? "ON" : (powerStatus == POWER_STATUS_STANDBY) ? "STANDBY" : (powerStatus == POWER_STATUS_BUS_ERROR) ? "BUS_ERROR" : "OFF");
    cJSON_AddStringToObject(status, "boost", (statusDigit3 & (0x01 << LED_BOOST)) >> LED_BOOST == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "sleep", (statusDigit3 & (0x01 << LED_SLEEP)) >> LED_SLEEP == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "o3_generation", (statusDigit3 & (0x01 << LED_OZONE)) >> LED_OZONE == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "pump_low_flow", (statusDigit3 & (0x01 << LED_PUMP_LOW_FLOW)) >> LED_PUMP_LOW_FLOW == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "low_salt", (statusDigit3 & (0x01 << LED_LOW_SALT)) >> LED_LOW_SALT == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "high_salt", (statusDigit3 & (0x01 << LED_HIGH_SALT)) >> LED_HIGH_SALT == 1 ? "ON" : "OFF");
    cJSON_AddStringToObject(status, "service", (statusDigit3 & (0x01 << LED_SERVICE)) >> LED_SERVICE == 1 ? "ON" : "OFF");
    cJSON_AddItemToObject(data, "status", status);

    cJSON *mode = cJSON_CreateObject();
    cJSON_AddBoolToObject(mode, "working", powerStatus == POWER_STATUS_ON);
    cJSON_AddBoolToObject(mode, "programming", displayBlinking);
    cJSON_AddItemToObject(data, "mode", mode);

    uint64_t time_us = esp_timer_get_time();
    uint32_t uptime_sec = (uint32_t)(time_us / 1000000ULL);
    cJSON *system = cJSON_CreateObject();
    cJSON_AddNumberToObject(system, "uptime_seconds", uptime_sec);
    cJSON_AddNumberToObject(system, "heap", esp_get_free_heap_size());
    cJSON_AddItemToObject(data, "system", system);

    cJSON_AddItemToObject(root, "data", data);
    
    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(root);

    return ESP_OK;
}

// HTTP GET General info request
static esp_err_t debug_get_handler(httpd_req_t *req){
    
    notifyApiRequest();
    httpd_resp_set_type(req, "application/json");
    
    std::string displayDigits;
    displayDigits += getDisplayDigitFromCode(displayingDigit2);
    displayDigits += getDisplayDigitFromCode(displayingDigit1);
    if (displayingDigit1 == DISP_1_CLEAN_06P || displayingDigit1 == DISP_1_CLEAN_10P || displayingDigit1 == DISP_1_CLEAN_14P) {
        displayDigits += '.';
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    char powerStatusChar[10];
    char statusDigit1Char[10];
    char statusDigit2Char[10];
    char statusDigit3Char[10];
    char displayingDigit1Char[10];
    char displayingDigit2Char[10];
    snprintf(powerStatusChar, sizeof(powerStatusChar), "0x%02X", powerStatus);
    snprintf(statusDigit1Char, sizeof(statusDigit1Char), "0x%02X", statusDigit1);
    snprintf(statusDigit2Char, sizeof(statusDigit2Char), "0x%02X", statusDigit2);
    snprintf(statusDigit3Char, sizeof(statusDigit3Char), "0x%02X", statusDigit3);
    snprintf(displayingDigit1Char, sizeof(displayingDigit1Char), "0x%02X", displayingDigit1);
    snprintf(displayingDigit2Char, sizeof(displayingDigit2Char), "0x%02X", displayingDigit2);

    // Build the received-bus dump on the heap (std::string grows as needed)
    // instead of a large fixed stack buffer, so we can't overflow the httpd
    // task stack regardless of buffer size.
    std::string readBufferStr;
    readBufferStr.reserve(128 * 16);
    char entry[24];
    for (int i = 0; i != 128; i++) {
        if (i) {
            readBufferStr += ", ";
        }
        snprintf(entry, sizeof(entry), "[0X%02X, 0X%02X]", dataReceivedBuffer[i][0], dataReceivedBuffer[i][1]);
        readBufferStr += entry;
    }

    cJSON_AddStringToObject(data, "powerStatus", powerStatusChar);
    cJSON_AddStringToObject(data, "statusDigit1", statusDigit1Char);
    cJSON_AddStringToObject(data, "statusDigit2", statusDigit2Char);
    cJSON_AddStringToObject(data, "statusDigit3", statusDigit3Char);
    cJSON_AddStringToObject(data, "displayingDigit1", displayingDigit1Char);
    cJSON_AddStringToObject(data, "displayingDigit2", displayingDigit2Char);
    cJSON_AddStringToObject(data, "current_code", displayDigits.c_str());
    cJSON_AddStringToObject(data, "readBuffer", readBufferStr.c_str());
    
    cJSON_AddStringToObject(data, "compilation_date", __DATE__);
    cJSON_AddStringToObject(data, "compilation_time", __TIME__);
    
    cJSON_AddBoolToObject(data, "otaUpdating", otaUpdating);
    cJSON_AddBoolToObject(data, "removeWifiConfig", removeWifiConfig);
    cJSON_AddBoolToObject(data, "machineON", machineON);
    cJSON_AddBoolToObject(data, "wifiReconnecting", wifiReconnecting);
    cJSON_AddBoolToObject(data, "readingMaster", readingMaster);
    cJSON_AddBoolToObject(data, "sendingKeyCode", sendingKeyCode);

    cJSON *picInjection = cJSON_CreateObject();
    cJSON_AddBoolToObject(picInjection, "active", picInjectionActive);
    cJSON_AddNumberToObject(picInjection, "phase", picInjectionCurrentPhase);
    cJSON_AddNumberToObject(picInjection, "last_tm_button", picInjectionLastTmButton);
    cJSON_AddNumberToObject(picInjection, "last_pic_button", picInjectionLastPicButton);
    cJSON_AddNumberToObject(picInjection, "frames_started", picInjectionFramesStarted);
    cJSON_AddNumberToObject(picInjection, "frames_completed", picInjectionFramesCompleted);
    cJSON_AddNumberToObject(picInjection, "frames_repeated", picInjectionFramesRepeated);
    cJSON_AddNumberToObject(picInjection, "timing_start_low_us", PIC_INJ_START_LOW_US);
    cJSON_AddNumberToObject(picInjection, "timing_zero_high_us", PIC_INJ_ZERO_HIGH_US);
    cJSON_AddNumberToObject(picInjection, "timing_one_high_us", PIC_INJ_ONE_HIGH_US);
    cJSON_AddNumberToObject(picInjection, "timing_bit_low_us", PIC_INJ_BIT_LOW_US);
    cJSON_AddNumberToObject(picInjection, "timing_end_low_us", PIC_INJ_END_LOW_US);
    cJSON_AddNumberToObject(picInjection, "timing_repeat_gap_us", PIC_INJ_REPEAT_GAP_US);
    cJSON_AddItemToObject(data, "pic_injection", picInjection);

    uint64_t time_us = esp_timer_get_time();
    uint32_t uptime_sec = (uint32_t)(time_us / 1000000ULL);
    cJSON_AddNumberToObject(data, "uptime_seconds", uptime_sec);
    cJSON_AddNumberToObject(data, "heap", esp_get_free_heap_size());
    
    cJSON_AddItemToObject(root, "data", data);
    
    const char *response = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(root);

    return ESP_OK;
}


/* Simple handler for power control */
static esp_err_t general_info_post_handler(httpd_req_t *req){

    bool responseStatus = false;
    char buffer[100];

    cJSON *root = parse_request_json(req, buffer, sizeof(buffer));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON* cjson_data = cJSON_GetObjectItem(root, "data");
    cJSON* power_item = cjson_data != NULL ? cJSON_GetObjectItem(cjson_data, "power") : NULL;
    if (power_item == NULL || power_item->valuestring == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing power");
        return ESP_FAIL;
    }

    char* power = power_item->valuestring;

    if (strcmp(power, "on") == 0) {
        responseStatus = apiCommandPower(true);
    }
    else if (strcmp(power, "off") == 0) {
#if defined(CONFIG_INTSWG_POWER_RELAY)
        responseStatus = apiCommandPower(false);
#else
        // No power relay wired: the SWG main board is always powered and
        // cannot be switched off, only put into standby.
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power off unavailable (no relay)");
        return ESP_FAIL;
#endif
    }
    else if (strcmp(power, "standby") == 0) {
        responseStatus = apiCommandStandby();
    }
    else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid power value");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", responseStatus);
    cJSON_AddItemToObject(responseRoot, "data", responseData);
    
    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req){

    bool responseStatus = false;
    char buffer[100];

    cJSON *root = parse_request_json(req, buffer, sizeof(buffer));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON* cjson_data = cJSON_GetObjectItem(root, "data");
    cJSON* reboot_item = cjson_data != NULL ? cJSON_GetObjectItem(cjson_data, "reboot") : NULL;
    if (reboot_item == NULL || reboot_item->valuestring == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing reboot");
        return ESP_FAIL;
    }

    char* myreboot = reboot_item->valuestring;
    responseStatus=false; 
    if (strcmp(myreboot, "yes") == 0) {
        responseStatus=true; 
        esp_restart();
    }
    
    //esp_restart();
    cJSON_Delete(root);
    
    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", responseStatus);
    cJSON_AddItemToObject(responseRoot, "data", responseData);
    
    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

static esp_err_t enableota_post_handler(httpd_req_t *req){

    bool responseStatus = false;
    char buffer[100];

    cJSON *root = parse_request_json(req, buffer, sizeof(buffer));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON* cjson_data = cJSON_GetObjectItem(root, "data");
    cJSON* enableota_item = cjson_data != NULL ? cJSON_GetObjectItem(cjson_data, "enableota") : NULL;
    if (enableota_item == NULL || enableota_item->valuestring == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enableota");
        return ESP_FAIL;
    }

    char* myreboot = enableota_item->valuestring;
    responseStatus=false; 
    if (strcmp(myreboot, "yes") == 0) {
        enableota=1; 
        responseStatus=true;
    }
    if (strcmp(myreboot, "no") == 0) {
        enableota=0; 
        responseStatus=false;
    }
    
    cJSON_Delete(root);
    
    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", responseStatus);
    cJSON_AddItemToObject(responseRoot, "data", responseData);
    
    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

static esp_err_t slef_clean_post_handler(httpd_req_t *req){

    bool responseStatus = false;
    char buffer[100];

    cJSON *root = parse_request_json(req, buffer, sizeof(buffer));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON* cjson_data = cJSON_GetObjectItem(root, "data");
    cJSON* time_item = cjson_data != NULL ? cJSON_GetObjectItem(cjson_data, "time") : NULL;
    if (time_item == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing time");
        return ESP_FAIL;
    }

    int selfCleanRequestedTime = time_item->valueint;

    uint8_t selfCleanCode;
    if (selfCleanRequestedTime <= 6) {
        selfCleanCode = DISP_1_CLEAN_06P;
    }
    else if (selfCleanRequestedTime <= 10) {
        selfCleanCode = DISP_1_CLEAN_10P;
    }
    else {
        selfCleanCode = DISP_1_CLEAN_14P;
    }

    responseStatus = apiCommandSelfClean(selfCleanCode);

    cJSON_Delete(root);

    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", responseStatus);
    cJSON_AddItemToObject(responseRoot, "data", responseData);
    
    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

/* Simple handler for display control */
static esp_err_t display_post_handler(httpd_req_t *req){
    char buffer[100];

    cJSON *root = parse_request_json(req, buffer, sizeof(buffer));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON* cjson_data = cJSON_GetObjectItem(root, "data");
    cJSON* brightness_item = cjson_data != NULL ? cJSON_GetObjectItem(cjson_data, "brightness") : NULL;
    if (brightness_item == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing brightness");
        return ESP_FAIL;
    }

    displayIntensity = brightness_item->valueint % 8; 
    ESP_LOGI(TAG, "Set display brightness to '%d'", displayIntensity);     

    cJSON_Delete(root);

    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", true);
    cJSON_AddItemToObject(responseRoot, "data", responseData);
    
    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

/* Simple handler for removing wifi config */
static esp_err_t wifi_config_delete_handler(httpd_req_t *req){
    notifyApiRequest();
    removeWifiConfig = true;

    // Response
    cJSON *responseRoot = cJSON_CreateObject();
    cJSON *responseData = cJSON_CreateObject();    
    cJSON_AddBoolToObject(responseData, "status", true);
    cJSON_AddItemToObject(responseRoot, "data", responseData);

    const char *response = cJSON_Print(responseRoot);
    httpd_resp_sendstr(req, response);
    free((void *)response);
    cJSON_Delete(responseRoot);

    return ESP_OK;
}

static const httpd_uri_t swg_status_get_uri = {
    .uri       = "/api/v1/intex/swg/status",
    .method    = HTTP_GET,
    .handler   = general_info_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t swg_debug_get_uri = {
    .uri       = "/api/v1/intex/swg/debug",
    .method    = HTTP_GET,
    .handler   = debug_get_handler,
    .user_ctx  = NULL
};

/* URI handler for power control */
static const httpd_uri_t swg_status_post_uri = {
    .uri = "/api/v1/intex/swg",
    .method = HTTP_POST,
    .handler = general_info_post_handler,
    .user_ctx = NULL
};

/* URI handler for power control */
static const httpd_uri_t swg_reboot_post_uri = {
    .uri = "/api/v1/intex/swg/reboot",
    .method = HTTP_POST,
    .handler = reboot_post_handler,
    .user_ctx = NULL
};

static const httpd_uri_t swg_enableota_post_uri = {
    .uri = "/api/v1/intex/swg/enableota",
    .method = HTTP_POST,
    .handler = enableota_post_handler,
    .user_ctx = NULL
};

/* URI handler for self clean control */
static const httpd_uri_t self_clean_post_uri = {
    .uri = "/api/v1/intex/swg/self_clean",
    .method = HTTP_POST,
    .handler = slef_clean_post_handler,
    .user_ctx = NULL
};

/* URI handler for display control */
static const httpd_uri_t display_post_uri = {
    .uri = "/api/v1/intex/swg/display",
    .method = HTTP_POST,
    .handler = display_post_handler,
    .user_ctx = NULL
};

/* URI handler for delete wifi config */
static const httpd_uri_t wifi_config_delete_uri = {
    .uri = "/api/v1/intex/swg/wifi",
    .method = HTTP_DELETE,
    .handler = wifi_config_delete_handler,
    .user_ctx = NULL
};

/******************* OTA ***********************************/

/*****************************************************
 
	systemRebootTask()
 
	NOTES: This had to be a task because the web page needed
			an ack back. So i could not call this in the handler
 
 *****************************************************/
void systemRebootTask(void * parameter){
	// Init the event group
	reboot_event_group = xEventGroupCreate();
	
	// Clear the bit
	xEventGroupClearBits(reboot_event_group, REBOOT_BIT);

	
	for (;;)
	{
		// Wait here until the bit gets set for reboot
		EventBits_t staBits = xEventGroupWaitBits(reboot_event_group, REBOOT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
		
		// Did portMAX_DELAY ever timeout, not sure so lets just check to be sure
		if ((staBits & REBOOT_BIT) != 0)
		{
			ESP_LOGI("OTA", "Reboot Command, Restarting");
			vTaskDelay(2000 / portTICK_PERIOD_MS);

			esp_restart();
		}
	}
}
/* Send index.html Page */
static esp_err_t OTA_index_html_handler(httpd_req_t *req)
{
	ESP_LOGI("OTA", "index.html Requested");

	// Clear this every time page is requested
	flash_status = 0;
	
	httpd_resp_set_type(req, "text/html");

	httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);

	return ESP_OK;
}

/* Status */
static esp_err_t OTA_update_status_handler(httpd_req_t *req)
{
	char ledJSON[100];
	
	ESP_LOGI("OTA", "Status Requested");
	
	snprintf(ledJSON, sizeof(ledJSON), "{\"status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\"}", flash_status, __TIME__, __DATE__);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, ledJSON, strlen(ledJSON));
	
	// This gets set when upload is complete
	if (flash_status == 1)
	{
		// We cannot directly call reboot here because we need the 
		// browser to get the ack back. 
		xEventGroupSetBits(reboot_event_group, REBOOT_BIT);		
	}

	return ESP_OK;
}
/* Receive .Bin file */
static esp_err_t OTA_update_post_handler(httpd_req_t *req)
{
    ESP_LOGI("OTA", "Update handler called");
    if(enableota==1){
    otaUpdating = true;
    
	esp_ota_handle_t ota_handle; 
	
	char ota_buff[1024];
	int content_length = req->content_len;
	int content_received = 0;
    uint8_t percentage = 0;
	int recv_len;
	bool is_req_body_started = false;
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    char complete_percent[12];

	// Unsucessful Flashing
	flash_status = -1;
	
	do
	{
		/* Read the data for the request */
		if ((recv_len = httpd_req_recv(req, ota_buff, MIN(content_length, sizeof(ota_buff)))) < 0) 
		{
			if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) 
			{
				ESP_LOGI("OTA", "Socket Timeout");
				/* Retry receiving if timeout occurred */
				continue;
			}
			ESP_LOGI("OTA", "OTA Other Error %d", recv_len);
			return ESP_FAIL;
		}

		//printf("OTA RX: %d of %d\r", content_received, content_length);
		
	    // Is this the first data we are receiving
		// If so, it will have the information in the header we need. 
		if (!is_req_body_started)
		{
			is_req_body_started = true;
			
			// Lets find out where the actual data staers after the header info		
			char *body_start_p = strstr(ota_buff, "\r\n\r\n") + 4;	
			int body_part_len = recv_len - (body_start_p - ota_buff);
			
			//int body_part_sta = recv_len - body_part_len;
			//printf("OTA File Size: %d : Start Location:%d - End Location:%d\r\n", content_length, body_part_sta, body_part_len);
			printf("OTA File Size: %d\r\n", content_length);

			esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
			if (err != ESP_OK) {
				printf("Error With OTA Begin, Cancelling OTA\r\n");
				return ESP_FAIL;
			} else {
                printf("Writing to partition subtype %d at offset 0x%lx\r\n", update_partition->subtype, (unsigned long)update_partition->address);
			}

			// Lets write this first part of data out
			esp_ota_write(ota_handle, body_start_p, body_part_len);
		} else {
			// Write OTA data
			esp_ota_write(ota_handle, ota_buff, recv_len);
			
			content_received += recv_len;
            percentage = MIN(content_received * 100 / content_length, 99);
            std::snprintf(complete_percent, sizeof(complete_percent), "%d", percentage);
            statusDigit1 = getCodeFromDisplayDigit((percentage < 10) ? complete_percent[0] : complete_percent[1]);
            statusDigit2 = getCodeFromDisplayDigit((percentage < 10) ? '0' : complete_percent[0]);
		}
 
	} while (recv_len > 0 && content_received < content_length);

	// End response
	// httpd_resp_send_chunk(req, NULL, 0);

	
	if (esp_ota_end(ota_handle) == ESP_OK) {
		// Lets update the partition
		if(esp_ota_set_boot_partition(update_partition) == ESP_OK) {
			const esp_partition_t *boot_partition = esp_ota_get_boot_partition();

			// Webpage will request status when complete 
			// This is to let it know it was successful
			flash_status = 1;
		
			ESP_LOGI("OTA", "Next boot partition subtype %d at offset 0x%x", boot_partition->subtype, boot_partition->address);
			ESP_LOGI("OTA", "Please Restart System...");
		} else {
			ESP_LOGI("OTA", "\r\n\r\n !!! Flashed Error !!!");
		}
		
	} else {
		ESP_LOGI("OTA", "\r\n\r\n !!! OTA End Error !!!");
	}
    
	return ESP_OK;
    } else {
        return ESP_FAIL;
    }
}

static const httpd_uri_t OTA_index_html = {
	.uri = "/",
	.method = HTTP_GET,
	.handler = OTA_index_html_handler,
	/* Let's pass response string in user
	 * context to demonstrate it's usage */
	.user_ctx = NULL
};

static const httpd_uri_t OTA_update = {
	.uri = "/update",
	.method = HTTP_POST,
	.handler = OTA_update_post_handler,
	.user_ctx = NULL
};

static const httpd_uri_t OTA_status = {
	.uri = "/status",
	.method = HTTP_POST,
	.handler = OTA_update_status_handler,
	.user_ctx = NULL
};

/******************* OTA ***********************************/


void start_rest_server(){
    if (server != NULL) {
        ESP_LOGI(TAG, "REST server already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769; 
    config.stack_size = 8192;
    config.max_uri_handlers = 13;
    // Allow several Home Assistant clients to be connected at once. The httpd
    // is still single-threaded (handlers are serialized via select()), but more
    // sockets means concurrent connections aren't dropped. LWIP default allows
    // up to 10 sockets, leaving headroom for the listen + ctrl sockets.
    config.max_open_sockets = 7;
    // Reap stale/half-open connections so a stuck client can't tie up a socket
    // forever (otherwise it would block other clients once all sockets are in use).
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    esp_err_t ret = httpd_start(&server, &config);
    if (ret == ESP_OK) { 
        // Set URI handlers
        register_server_uri_handlers();
    } 
    else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
    }
}

void register_server_uri_handlers(){
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &swg_status_get_uri);
        httpd_register_uri_handler(server, &swg_debug_get_uri);
        httpd_register_uri_handler(server, &swg_status_post_uri);
        httpd_register_uri_handler(server, &swg_reboot_post_uri);
        httpd_register_uri_handler(server, &swg_enableota_post_uri);
        httpd_register_uri_handler(server, &self_clean_post_uri);
        httpd_register_uri_handler(server, &display_post_uri);
        httpd_register_uri_handler(server, &wifi_config_delete_uri);

        httpd_register_uri_handler(server, &OTA_index_html);
		httpd_register_uri_handler(server, &OTA_update);
		httpd_register_uri_handler(server, &OTA_status);
}