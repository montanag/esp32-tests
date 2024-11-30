#include "BleCharacteristic.h"

static const char* TAG = "BLE_CHARACTERISTIC";

////////////////////////////////////////////////////////////////////////////////
/// Constructors / Destructors
////////////////////////////////////////////////////////////////////////////////

BleCharacteristic::BleCharacteristic(
    string uuid,
    function<int(vector<byte>)> onWrite,
    function<vector<byte>(void)> onRead,
    function<void(shared_ptr<BleDevice>)> onSubscribe,
    bool acknowledgeWrites
):
    onWrite(onWrite),
    onRead(onRead),
    onSubscribe(onSubscribe),
    acknowledgeWrites(acknowledgeWrites)
{
    ESP_LOGW(TAG, "constructor called");

    // Set the read and write flags
    read = onRead != nullptr;
    write = onWrite != nullptr;

    // Populate the UUID structure from the UUID string
    ESP_ERROR_CHECK(uuidStringToUuid(uuid, this->uuidDefinition));
};


// void print16ByteUUID(ble_uuid_any_t uuid) {
//     for (int i = 0; i < 16; i++) {
//         printf("%02x", uuid.u128.value[i]);
//     }
// }


BleCharacteristic::BleCharacteristic(BleCharacteristic&& other) {
    ESP_LOGW(TAG, "move constructor called");
    this->uuidDefinition = std::move(other.uuidDefinition);
    this->onWrite = std::move(other.onWrite);
    this->onRead = std::move(other.onRead);
    this->onSubscribe = std::move(other.onSubscribe);
    this->acknowledgeWrites = std::move(other.acknowledgeWrites);

    // // Log both the old and new values in a table-like format
    // ESP_LOGI(TAG, "old value\tnew value");
    // ESP_LOGI(TAG, "Uuid: ");
    // print16ByteUUID(other.uuidDefinition);
    // printf("\t");
    // print16ByteUUID(this->uuidDefinition);
    // printf("\n");

    // // Log whether the functions are set or empty
    // ESP_LOGI(TAG, "onWrite: %s\t%s\n", other.onWrite ? "set" : "empty", this->onWrite ? "set" : "empty");
    // ESP_LOGI(TAG, "onRead: %s\t%s\n", other.onRead ? "set" : "empty", this->onRead ? "set" : "empty");
    // ESP_LOGI(TAG, "onSubscribe: %s\t%s\n", other.onSubscribe ? "set" : "empty", this->onSubscribe ? "set" : "empty");
    // ESP_LOGI(TAG, "acknowledgeWrites: %d\t%d\n", other.acknowledgeWrites, this->acknowledgeWrites);
}

BleCharacteristic::~BleCharacteristic()
{
    ESP_LOGW(TAG, "destructor called");
    delete characteristicHandle;
}

////////////////////////////////////////////////////////////////////////////////
/// Public functions
////////////////////////////////////////////////////////////////////////////////

esp_err_t BleCharacteristic::notify(vector<shared_ptr<BleDevice>> devices, vector<byte> data) {

    os_mbuf* om = os_msys_get_pkthdr(data.size(), 0);
    // TODO: Doc and error handling. the above could throw

    // Copy the data to the nimble stack
    int response = os_mbuf_append(om, data.data(), data.size());

    // TODO: Iterate through devices
    ble_gatts_notify_custom(
        devices[0]->connectionHandle,
        *characteristicHandle,
        om
    );

    return ESP_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Friend functions
////////////////////////////////////////////////////////////////////////////////

esp_err_t BleCharacteristic::populateGattCharacteristicDefinition(ble_gatt_chr_def* gattCharacteristicDefinition) {
    // Populate the flags
    ble_gatt_chr_flags flags = 0;
    flags = flags | (read ? BLE_GATT_CHR_F_READ : 0);
    ble_gatt_chr_flags acknowledgeWritesFlag = acknowledgeWrites ? BLE_GATT_CHR_F_WRITE : BLE_GATT_CHR_F_WRITE_NO_RSP;
    flags = write ? (flags | acknowledgeWritesFlag) : flags;
    // TODO: Add support for notify and indicate. This should be passed in from the constructor
    flags = flags | BLE_GATT_CHR_F_NOTIFY;

    *gattCharacteristicDefinition = (struct ble_gatt_chr_def)
    {
        .uuid = &this->uuidDefinition.u,
        .access_cb = characteristicAccessHandler,
        .arg = this,
        .flags = flags,
        .val_handle = characteristicHandle,
    };

    return ESP_OK;
}

uint16_t* BleCharacteristic::getHandle() {
    return characteristicHandle;
}

////////////////////////////////////////////////////////////////////////////////
/// Private functions
////////////////////////////////////////////////////////////////////////////////

int BleCharacteristic::characteristicAccessHandler
(
    uint16_t connectionHandle,
    uint16_t attributeHandle,
    struct ble_gatt_access_ctxt *gattAccessContext,
    void *arg
) {
    // Get the characteristic from the argument
    BleCharacteristic* characteristic = (BleCharacteristic*) arg;

    // Handle access events
    switch (gattAccessContext->op) {
        // Read characteristic
        case BLE_GATT_ACCESS_OP_READ_CHR: {
            // Verify the connection handle
            if (connectionHandle != BLE_HS_CONN_HANDLE_NONE) {
                // The characteristic was read by a connected device
                ESP_LOGI(TAG, "characteristic read; conn_handle=%d attr_handle=%d",
                        connectionHandle, attributeHandle);
            } else {
                // The characteristic was read by the nimble stack
                ESP_LOGI(TAG,
                        "characteristic read by nimble stack; attr_handle=%d",
                        attributeHandle);
            }

            // Check if the characteristic handle is the same as the attribute handle
            if (*characteristic->characteristicHandle != attributeHandle) {
                ESP_LOGE(TAG, "unknown attribute handle: %d", attributeHandle);
                return BLE_ATT_ERR_UNLIKELY;
            }

            // Check if the characteristic has a read callback
            if (characteristic->onRead == nullptr) {
                ESP_LOGE(TAG, "characteristic does not have a read callback");
                return BLE_ATT_ERR_UNLIKELY;
            }

            // Get the data from the characteristic
            vector<byte> data = characteristic->onRead();

            // Copy the data to the nimble stack
            int response = os_mbuf_append(gattAccessContext->om, data.data(), data.size());

            // Return the response
            return response == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        // Write characteristic
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            // Verify the connection handle
            if (connectionHandle != BLE_HS_CONN_HANDLE_NONE) {
                // The characteristic was written to by a connected device
                ESP_LOGI(TAG, "characteristic write; conn_handle=%d attr_handle=%d",
                        connectionHandle, attributeHandle);
            } else {
                // The characteristic was written to by the nimble stack
                ESP_LOGI(TAG,
                        "characteristic write by nimble stack; attr_handle=%d",
                        attributeHandle);
            }

            // Check if the characteristic handle is the same as the attribute handle
            if (*characteristic->characteristicHandle != attributeHandle) {
                ESP_LOGE(TAG, "unknown attribute handle: %d", attributeHandle);
                return BLE_ATT_ERR_UNLIKELY;
            }

            // Check if the characteristic has a write callback
            if (characteristic->onWrite == nullptr) {
                ESP_LOGE(TAG, "characteristic does not have a write callback");
                return BLE_ATT_ERR_UNLIKELY;
            }

            // Convert the data to a vector of bytes
            vector<byte> data = vector<byte>(
                (byte*) gattAccessContext->om->om_data,
                (byte*) gattAccessContext->om->om_data + gattAccessContext->om->om_len
            );

            // Call the write callback
            return characteristic->onWrite(data);
        }
        // Read descriptor
        case BLE_GATT_ACCESS_OP_READ_DSC:
            ESP_LOGE(TAG, "operation not implemented, opcode: %d", gattAccessContext->op);
            return BLE_ATT_ERR_UNLIKELY;
        // Write descriptor
        case BLE_GATT_ACCESS_OP_WRITE_DSC:
            ESP_LOGE(TAG, "operation not implemented, opcode: %d", gattAccessContext->op);
            return BLE_ATT_ERR_UNLIKELY;
        // Unknown event
        default:
            ESP_LOGE(TAG,
                "unexpected access operation to led characteristic, opcode: %d",
                gattAccessContext->op);
            return BLE_ATT_ERR_UNLIKELY;
    }

    // Control shouldn't reach here
}










// These function should be moved to a helper class

esp_err_t BleCharacteristic::hexStringToUint8(const string& hexStr, uint8_t& result) {
    if (hexStr.size() != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    // Convert the string to an integer using stoi with base 16
    int value = stoi(hexStr, nullptr, 16);

    // Ensure the value fits in a uint8_t
    if (value < 0 || value > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    // Set the result and return
    result = static_cast<uint8_t>(value);
    return ESP_OK;
};

esp_err_t BleCharacteristic::uuidStringToUuid(string uuid, ble_uuid_any_t& result) {
    // Remove all dashes from string
    uuid.erase(remove(uuid.begin(), uuid.end(), '-'), uuid.end());

    switch (uuid.size()) {
        // 128-bit UUID
        case 32:
            {
                // Set the type of the result (are both of these necessary?)
                result.u.type = BLE_UUID_TYPE_128;
                result.u128.u.type = BLE_UUID_TYPE_128;

                // uint8_t uuidBytes[16];
                for (size_t i = 0; i < uuid.size(); i += 2) {
                    string hexString = uuid.substr(i, 2);

                    uint8_t val;
                    ESP_ERROR_CHECK(hexStringToUint8(hexString, val));
                    
                    // Populate the UUID bytes in reverse order
                    result.u128.value[15 - (i / 2)] = val;
                }

                return ESP_OK;
            }
        case 8:
            // TODO: Implement 32-bit UUID (these are of the hexidecimal form 00000000)
            break;
        case 4:
            // TODO: Implement 16-bit UUID (these are of the hexidecimal form 0000)
            break;
        default:
            // TODO: Implement error handling
            break;
    }

    // return ble_uuid_any_t();
    return ESP_FAIL;
}