#include "BleAdvertiser.h"

#define BLE_GAP_URI_PREFIX_HTTPS 0x17
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};
static char* TAG = "BLE_ADVERTISER";

// Define static members to satisfy the compiler. These will be overwritten by the init function
std::string BleAdvertiser::deviceName = "esp32_bluetooth";
uint16_t BleAdvertiser::deviceAppearance = 0;
uint8_t BleAdvertiser::deviceRole = 0;
bool BleAdvertiser::initiated = false;
std::map<uint16_t*, BleCharacteristic> BleAdvertiser::characteristicHandlesToCharacteristics = {};
uint8_t BleAdvertiser::deviceAddress[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t BleAdvertiser::deviceAddressType = 0;
uint16_t BleAdvertiser::mtu = 0;

////////////////////////////////////////////////////////////////////////////
// Public functions
////////////////////////////////////////////////////////////////////////////

bool BleAdvertiser::init(
    std::string deviceName,
    uint16_t deviceAppearance,
    uint8_t deviceRole,
    std::vector<BleService> services
) {
    // Initialize static variables
    BleAdvertiser::deviceName = deviceName;
    BleAdvertiser::deviceAppearance = deviceAppearance;
    BleAdvertiser::deviceRole = deviceRole;

    // Initialise the non-volatile flash storage (NVS)
    ESP_LOGI(TAG, "initializing nvs flash");
    esp_err_t response = nvs_flash_init();

    // Attempt to recover if an error occurred
    if (response != ESP_OK) {
        // Check if a recoverable error occured
        if (response == ESP_ERR_NVS_NO_FREE_PAGES ||
            response == ESP_ERR_NVS_NEW_VERSION_FOUND) {

            // Erase and re-try if necessary
            // Note: This will erase the nvs flash.
            // TODO: We should consider alternative impls here. Erasing the NVS could be a very unwanted side-effect
            ESP_LOGI(TAG, "erasing nvs flash");
            ESP_ERROR_CHECK(nvs_flash_erase());
            response = nvs_flash_init();

            if (response != ESP_OK) {
                ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", response);
                return false;
            };
        }
    }

    // Initialise the controller and nimble host stack 
    response = nimble_port_init();
    if (response != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
            response);
        return false;
    }

    // Initialize the Generic Attribute Profile (GAP)
    response = gapInit();
    if (response != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", response);
        return false;
    }

    // Create the GATT services
    struct ble_gatt_svc_def *serviceDefinitions = createServiceDefinitions(services);

    // Initialize the Generic ATTribute Profile (GATT) server <- I know, it's a bad acronym
    response = gattSvcInit(serviceDefinitions);
    if (response != 0) {
        ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", response);
        return false;
    }

    // Initialize the nimble host configuration
    nimbleHostConfigInit();

    // Set the initialized flag to true
    initiated = true;

    return true;
}

void BleAdvertiser::advertise(void) {
    // Log the start of the task
    ESP_LOGI(TAG, "nimble host task has been started by %s", deviceName.c_str());

    // This function won't return until nimble_port_stop() is executed
    nimble_port_run();

    // Clean up at exit
    vTaskDelete(NULL);
}

uint16_t BleAdvertiser::getMtu(void) {
    return BleAdvertiser::mtu;
}

////////////////////////////////////////////////////////////////////////////
// Characteristic access handler
////////////////////////////////////////////////////////////////////////////

int BleAdvertiser::characteristicAccessHandler
(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
) {
    // Handle access events
    switch (ctxt->op) {
        // Read characteristic
        case BLE_GATT_ACCESS_OP_READ_CHR:
            // Verify the connection handle
            if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                // The characteristic was read by a connected device
                ESP_LOGI(TAG, "characteristic read; conn_handle=%d attr_handle=%d",
                        conn_handle, attr_handle);
            } else {
                // The characteristic was read by the nimble stack
                ESP_LOGI(TAG,
                        "characteristic read by nimble stack; attr_handle=%d",
                        attr_handle);
            }

            // Find the callback for reading the characteristic
            for (const auto& [characteristicHandle, characteristic] : characteristicHandlesToCharacteristics) {
                if (*characteristicHandle == attr_handle) {
                    // Get the data from the characteristic
                    std::vector<std::byte> data = characteristic.onRead();

                    // Copy the data to the nimble stack
                    int response = os_mbuf_append(ctxt->om, data.data(), data.size());

                    // Return the response
                    return response == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            }

            ESP_LOGE(TAG, "unknown attribute handle: %d", attr_handle);
            return BLE_ATT_ERR_UNLIKELY;
        // Write characteristic
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            // Verify the connection handle
            if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                // The characteristic was written to by a connected device
                ESP_LOGI(TAG, "characteristic write; conn_handle=%d attr_handle=%d",
                        conn_handle, attr_handle);
            } else {
                // The characteristic was written to by the nimble stack
                ESP_LOGI(TAG,
                        "characteristic write by nimble stack; attr_handle=%d",
                        attr_handle);
            }

            // Find the callback for writing to the characteristic
            for (const auto& [characteristicHandle, characteristic] : characteristicHandlesToCharacteristics) {
                if (*characteristicHandle == attr_handle) {
                    // Convert the data to a vector of bytes
                    std::vector<std::byte> data = std::vector<std::byte>(
                        (std::byte*) ctxt->om->om_data,
                        (std::byte*) ctxt->om->om_data + ctxt->om->om_len
                    );

                    return characteristic.onWrite(data);
                }
            }

            ESP_LOGE(TAG, "unknown attribute handle: %d", attr_handle);
            return BLE_ATT_ERR_UNLIKELY;
        // Read descriptor
        case BLE_GATT_ACCESS_OP_READ_DSC:
            ESP_LOGE(TAG, "operation not implemented, opcode: %d", ctxt->op);
            return BLE_ATT_ERR_UNLIKELY;
        // Write descriptor
        case BLE_GATT_ACCESS_OP_WRITE_DSC:
            ESP_LOGE(TAG, "operation not implemented, opcode: %d", ctxt->op);
            return BLE_ATT_ERR_UNLIKELY;
        // Unknown event
        default:
            ESP_LOGE(TAG,
                "unexpected access operation to led characteristic, opcode: %d",
                ctxt->op);
            return BLE_ATT_ERR_UNLIKELY;
    }

    // Control shouldn't reach here
}

////////////////////////////////////////////////////////////////////////////
// BLE helper functions
////////////////////////////////////////////////////////////////////////////

inline void BleAdvertiser::formatAddress(char *addressString, uint8_t address[]) {
    sprintf(addressString, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1],
            address[2], address[3], address[4], address[5]);
}

void BleAdvertiser::printConnectionDescription(struct ble_gap_conn_desc *connectionDescription) {
    // Local variables to be reused
    char addressString[18] = {0};

    // Connection handle
    ESP_LOGI(TAG, "connection handle: %d", connectionDescription->conn_handle);

    // Local ID address
    formatAddress(addressString, connectionDescription->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             connectionDescription->our_id_addr.type, addressString);

    // Peer ID address
    formatAddress(addressString, connectionDescription->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", connectionDescription->peer_id_addr.type,
             addressString);

    // Connection info
    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             connectionDescription->conn_itvl, connectionDescription->conn_latency, connectionDescription->supervision_timeout,
             connectionDescription->sec_state.encrypted, connectionDescription->sec_state.authenticated,
             connectionDescription->sec_state.bonded);
}

int BleAdvertiser::gapInit(void) {
    // Local variables to be reused
    int response = 0;

    // Initialize the gap service
    ble_svc_gap_init();

    // Set the gap device name
    response = ble_svc_gap_device_name_set(deviceName.c_str());
    if (response != 0) {
        ESP_LOGE(TAG , "failed to set device name to %s, error code: %d",
                 deviceName.c_str(), response);
        return response;
    }

    // Set the gap appearance
    response = ble_svc_gap_device_appearance_set(deviceAppearance);
    if (response != 0) {
        ESP_LOGE(TAG, "failed to set device appearance, error code: %d", response);
        return response;
    }
    return response;
}

int BleAdvertiser::gattSvcInit(struct ble_gatt_svc_def *serviceDefinitions) {
    // Local variables to reuse
    int response;

    // 1. GATT service initialization
    ble_svc_gatt_init();

    // 2. Update GATT services counter
    response = ble_gatts_count_cfg(serviceDefinitions);
    if (response != 0) {
        return response;
    }

    // 3. Add GATT services
    response = ble_gatts_add_svcs(serviceDefinitions);
    if (response != 0) {
        return response;
    }

    return 0;
}

void BleAdvertiser::nimbleHostConfigInit(void) {
    // Set the host callbacks
    // Idk where the ble_hs_cfg variable is are coming from?
    ble_hs_cfg.reset_cb = onStackReset;
    ble_hs_cfg.sync_cb = onStackSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.gatts_register_cb = gattSvrRegisterCb;

    // Store host configuration
    ble_store_config_init();
}

int BleAdvertiser::gapEventHandler(struct ble_gap_event *event, void *arg) {
    // Local variables to be reused
    int response = 0;
    struct ble_gap_conn_desc connectionDescription;

    // Handle different GAP events
    switch (event->type) {
        // Connect event
        case BLE_GAP_EVENT_CONNECT:
            // A new connection was established or a connection attempt failed.
            ESP_LOGI(TAG, "connection %s; status=%d",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);

            // Connection succeeded
            if (event->connect.status == 0) {
                // Check connection handle
                response = ble_gap_conn_find(event->connect.conn_handle, &connectionDescription);
                if (response != 0) {
                    ESP_LOGE(TAG,
                            "failed to find connection by handle, error code: %d",
                            response);
                    return response;
                }

                // Print connection descriptor
                printConnectionDescription(&connectionDescription);

                // Try to update connection parameters
                struct ble_gap_upd_params params = {.itvl_min = connectionDescription.conn_itvl,
                                                    .itvl_max = connectionDescription.conn_itvl,
                                                    .latency = 3,
                                                    .supervision_timeout =
                                                        connectionDescription.supervision_timeout};
                response = ble_gap_update_params(event->connect.conn_handle, &params);
                if (response != 0) {
                    ESP_LOGE(
                        TAG,
                        "failed to update connection parameters, error code: %d",
                        response);
                    return response;
                }

                // Exchange MTU
                ble_gattc_exchange_mtu(event->connect.conn_handle, mtuEventHandler, NULL);
            }
            // Connection failed, restart advertising
            else
            {
                startAdvertising();
            }
            return response;

        // Disconnect event
        case BLE_GAP_EVENT_DISCONNECT:
            // A connection was terminated, print connection descriptor
            ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                    event->disconnect.reason);

            // Restart advertising
            startAdvertising();
            return response;

        // Connection parameters update event
        case BLE_GAP_EVENT_CONN_UPDATE:
            // The central has updated the connection parameters.
            ESP_LOGI(TAG, "connection updated; status=%d",
                        event->conn_update.status);

            // Print connection descriptor
            response = ble_gap_conn_find(event->conn_update.conn_handle, &connectionDescription);
            if (response != 0) {
                ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                            response);
                return response;
            }
            printConnectionDescription(&connectionDescription);
            return response;
    }
    return response;
}

int BleAdvertiser::mtuEventHandler(uint16_t conn_handle, const ble_gatt_error *error, uint16_t mtu, void *arg){
    ESP_LOGI(TAG, "MTU exchanged. MTU set to %d", mtu);

    // Set the MTU
    BleAdvertiser::mtu = mtu;

    return 0;
}

////////////////////////////////////////////////////////////////////////////
// Nimble stack event callback functions
////////////////////////////////////////////////////////////////////////////

void BleAdvertiser::onStackReset(int reason) {
    // On reset, print reset reason to console
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

void BleAdvertiser::onStackSync(void) {
    // This function will be called once the nimble host stack is synced with the BLE controller
    // Once the stack is synced, we can do advertizing initialization and begin advertising

    // Initialize and start advertizing
    initializeAdvertising();
}

void BleAdvertiser::gattSvrRegisterCb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    // Local variables to be reused
    char buf[BLE_UUID_STR_LEN];

    // Handle GATT attributes register events
    switch (ctxt->op) {

        // Service register event
        case BLE_GATT_REGISTER_OP_SVC:
            ESP_LOGD(TAG, "registered service %s with handle=%d",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
            break;

        // Characteristic register event
        case BLE_GATT_REGISTER_OP_CHR:
            ESP_LOGD(TAG,
                    "registering characteristic %s with "
                    "def_handle=%d val_handle=%d",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle, ctxt->chr.val_handle);
            break;

        // Descriptor register event
        case BLE_GATT_REGISTER_OP_DSC:
            ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
            break;

        // Unknown event
        default:
            assert(0);
            break;
    }
}

////////////////////////////////////////////////////////////////////////////
// Advertising helper functions
////////////////////////////////////////////////////////////////////////////

void BleAdvertiser::initializeAdvertising(void) {
    // Local variables to be reused
    int rc = 0;
    char addressString[18] = {0};

    // Make sure we have a proper BT address
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    // Determine the BL address type to use while advertizing
    rc = ble_hs_id_infer_auto(0, &deviceAddressType);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    // Copy the device address to deviceAddress
    rc = ble_hs_id_copy_addr(deviceAddressType, deviceAddress, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    formatAddress(addressString, deviceAddress);
    ESP_LOGI(TAG, "device address: %s", addressString);

    // Start advertising.
    startAdvertising();
}

void BleAdvertiser::startAdvertising(void) {
    // Local variables to be reused
    int rc = 0;
    const char *name;
    struct ble_hs_adv_fields advertisingFields = {0};
    struct ble_hs_adv_fields responseFields = {0};
    struct ble_gap_adv_params advertisingParams = {0};

    // Set advertising flags
    advertisingFields.flags =
        BLE_HS_ADV_F_DISC_GEN | // advertising is general discoverable
        BLE_HS_ADV_F_BREDR_UNSUP; // BLE support only (BR/EDR refers to Bluetooth Classic)

    // Set device name
    name = ble_svc_gap_device_name(); // This was previously set in the GAP initialization step
    advertisingFields.name = (uint8_t *)name;
    advertisingFields.name_len = strlen(name);
    advertisingFields.name_is_complete = 1;

    // Set device tx power
    advertisingFields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    advertisingFields.tx_pwr_lvl_is_present = 1;

    // Set device appearance
    advertisingFields.appearance = deviceAppearance; // 4 bytes
    advertisingFields.appearance_is_present = 1;

    // Set device LE role
    advertisingFields.le_role = deviceRole; // 2 bytes
    advertisingFields.le_role_is_present = 1;

    /*
        It would be great if we could directly get the size of the advertizing packet,
        but unfortunately the function that makes this size available (adv_set_fields)
        appears to be private, though I could be wrong
    */
    // uint8_t buf[BLE_HS_ADV_MAX_SZ];
    // uint8_t buf_sz;
    // adv_set_fields(&advertisingFields, buf, &buf_sz, sizeof buf);
    // printf("len: %d", buf_sz);

    // Set advertisement fields
    rc = ble_gap_adv_set_fields(&advertisingFields);
    if (rc != 0) {
        if (rc == BLE_HS_EMSGSIZE) {
            ESP_LOGE(TAG, "failed to set advertising data, message data too long. Maximum advertizing packet size is %d", BLE_HS_ADV_MAX_SZ);
            return;
        }

        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    // Set device address
    responseFields.device_addr = deviceAddress;
    responseFields.device_addr_type = deviceAddressType;
    responseFields.device_addr_is_present = 1;

    // Set URI
    responseFields.uri = esp_uri;
    responseFields.uri_len = sizeof(esp_uri);

    // Set advertising interval in the response packet
    responseFields.adv_itvl = BLE_GAP_ADV_ITVL_MS(500); // unit of advertising interval is 0.625ms so we use this function to convert to ms
    responseFields.adv_itvl_is_present = 1; // unit of advertising interval is 0.625ms so we use this function to convert to ms

    // Set scan response fields
    rc = ble_gap_adv_rsp_set_fields(&responseFields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    // Set connectable and general discoverable mode
    advertisingParams.conn_mode = BLE_GAP_CONN_MODE_UND;
    advertisingParams.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // Set advertising interval in the advertizing packet
    advertisingParams.itvl_min = BLE_GAP_ADV_ITVL_MS(500); // unit of advertising interval is 0.625ms so we use this function to convert to ms
    advertisingParams.itvl_max = BLE_GAP_ADV_ITVL_MS(510); // unit of advertising interval is 0.625ms so we use this function to convert to ms

    // Start advertising
    rc = ble_gap_adv_start(deviceAddressType, NULL, BLE_HS_FOREVER, &advertisingParams,
                           gapEventHandler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started!");
}

////////////////////////////////////////////////////////////////////////////
// Service and characteristic definition builders
////////////////////////////////////////////////////////////////////////////

struct ble_gatt_svc_def* BleAdvertiser::createServiceDefinitions(const std::vector<BleService>& services) {
    // Get the number of services
    size_t servicesLength = services.size();

    // Allocate memory for the ble_gatt_svc_def array + 1 for the terminator
    // TODO: This is a memory leak. We should free this memory when we're done with it
    struct ble_gatt_svc_def* gattServices = (struct ble_gatt_svc_def*)malloc((servicesLength + 1) * sizeof(struct ble_gatt_svc_def));

    // Check if malloc was successful
    if (gattServices == nullptr) {
        return nullptr;
    }

    // Create each service
    for (size_t index = 0; index < servicesLength; index++) {
        gattServices[index] = createServiceDefinition(services[index]);
    }

    // Add the terminator { 0 } at the end
    gattServices[servicesLength] = (struct ble_gatt_svc_def){ 0 };

    return gattServices;
}

struct ble_gatt_svc_def BleAdvertiser::createServiceDefinition(BleService service) {
    return (struct ble_gatt_svc_def) {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service.getUuidPointer()->u,
        .characteristics = createCharacteristicDefinitions(service.characteristics)
    };
}

struct ble_gatt_chr_def* BleAdvertiser::createCharacteristicDefinitions(std::vector<BleCharacteristic> characteristics){
    // Get the number of characteristics
    size_t characteristicsLength = characteristics.size();

    // Check if there are any characteristics
    if (characteristicsLength == 0) {
        ESP_LOGW(TAG, "service has no characteristics. This service may not be discoverable by all consumers, eg web bluetooth");
    }

    // Allocate memory for the ble_gatt_chr_def array + 1 for the terminator
    struct ble_gatt_chr_def* gattCharacteristics = (ble_gatt_chr_def*)malloc((characteristicsLength + 1) * sizeof(struct ble_gatt_chr_def));

    // Check if malloc was successful
    if (gattCharacteristics == nullptr) {
        return nullptr;
    }

    // Create each characteristic
    for (int index = 0; index < characteristicsLength; index++) {
        gattCharacteristics[index] = createCharacteristicDefinition(characteristics[index]);
    }

    // Add the terminator { 0 } at the end
    gattCharacteristics[characteristicsLength] = (struct ble_gatt_chr_def){ 0 };

    return gattCharacteristics;
}

struct ble_gatt_chr_def BleAdvertiser::createCharacteristicDefinition(BleCharacteristic characteristic) {
    // First create the characteristic handle and link it to it's callback
    uint16_t* characteristicHandle = new uint16_t(0);
    characteristicHandlesToCharacteristics.insert(std::pair<uint16_t*, BleCharacteristic>(characteristicHandle, characteristic));

    // Populate the flags
    ble_gatt_chr_flags flags = 0;
    flags = flags | (characteristic.read ? BLE_GATT_CHR_F_READ : 0);
    ble_gatt_chr_flags acknowledgeWrites = characteristic.acknowledgeWrites ? BLE_GATT_CHR_F_WRITE : BLE_GATT_CHR_F_WRITE_NO_RSP;
    flags = characteristic.write ? (flags | acknowledgeWrites) : flags;

    return (struct ble_gatt_chr_def)
    {
        .uuid = &characteristic.getUuidPointer()->u,
        .access_cb = characteristicAccessHandler,
        .flags = flags,
        .val_handle = characteristicHandle,
    };
}
