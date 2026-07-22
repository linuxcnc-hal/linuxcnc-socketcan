#include <errno.h>
#include <expat.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <rtapi.h>
#include <hal.h>

#define LSC_DEFAULT_COMPONENT "lsc_canopen"
#define LSC_DEFAULT_INTERFACE "can0"
#define LSC_DEFAULT_PERIOD_US 1000U
#define LSC_DEFAULT_SDO_TIMEOUT_MS 500U
#define LSC_RECONNECT_DELAY_MS 1000U
#define LSC_MAX_NODES 16U
#define LSC_MAX_PDOS 4U
#define LSC_MAX_ENTRIES 16U
#define LSC_NAME_LEN 64U
#define LSC_PATH_LEN 512U
#define LSC_XML_BUFFER_SIZE 4096U
#define LSC_MAX_RX_FRAMES 256U
#define LSC_PDO_WRITE_TIMEOUT_MS 10U

typedef enum {
    LSC_PDO_NONE = 0,
    LSC_PDO_RPDO,
    LSC_PDO_TPDO,
} lsc_pdo_direction_t;

typedef enum {
    LSC_HAL_BIT = 0,
    LSC_HAL_U32,
    LSC_HAL_S32,
    LSC_HAL_FLOAT,
} lsc_hal_type_t;

typedef struct {
    char name[LSC_NAME_LEN];
    uint16_t index;
    uint8_t subindex;
    uint8_t bit_length;
    uint8_t bit_offset;
    lsc_hal_type_t hal_type;
    bool raw_signed;
    double scale;
    double offset;
} lsc_entry_config_t;

typedef struct {
    bool defined;
    uint8_t number;
    uint16_t cob_id;
    uint8_t transmission_type;
    uint16_t event_timer_ms;
    bool event_timer_set;
    uint32_t period_ms;
    uint8_t data_length;
    unsigned int entry_count;
    lsc_entry_config_t entries[LSC_MAX_ENTRIES];
} lsc_pdo_config_t;

typedef struct {
    char name[LSC_NAME_LEN];
    uint8_t node_id;
    bool config_pdos;
    bool start_node;
    uint16_t heartbeat_producer_ms;
    uint32_t heartbeat_timeout_ms;
    lsc_pdo_config_t rpdos[LSC_MAX_PDOS];
    lsc_pdo_config_t tpdos[LSC_MAX_PDOS];
} lsc_node_config_t;

typedef struct {
    char interface_name[IFNAMSIZ];
    uint32_t period_us;
    uint32_t sync_period_us;
    uint32_t sdo_timeout_ms;
    unsigned int node_count;
    lsc_node_config_t nodes[LSC_MAX_NODES];
} lsc_config_t;

typedef union {
    hal_bit_t *bit;
    hal_u32_t *u32;
    hal_s32_t *s32;
    hal_float_t *real;
    void *pointer;
} lsc_pin_t;

typedef struct {
    lsc_pin_t value;
} lsc_entry_hal_t;

typedef struct {
    hal_bit_t *send;
    hal_u32_t *tx_count;
    hal_s32_t *last_error;
    bool previous_send;
    uint64_t next_due_us;
    lsc_entry_hal_t entries[LSC_MAX_ENTRIES];
} lsc_rpdo_hal_t;

typedef struct {
    hal_u32_t *rx_count;
    hal_s32_t *last_error;
    lsc_entry_hal_t entries[LSC_MAX_ENTRIES];
} lsc_tpdo_hal_t;

typedef struct {
    hal_bit_t *enable;
    hal_bit_t *online;
    hal_bit_t *operational;
    hal_bit_t *config_ok;
    hal_u32_t *nmt_state;
    hal_u32_t *heartbeat_age_ms;
    hal_u32_t *sdo_abort;
    uint64_t last_heartbeat_us;
    lsc_rpdo_hal_t rpdos[LSC_MAX_PDOS];
    lsc_tpdo_hal_t tpdos[LSC_MAX_PDOS];
} lsc_node_hal_t;

typedef struct {
    hal_bit_t *connected;
    hal_bit_t *config_ok;
    hal_bit_t *bus_off;
    hal_u32_t *error_count;
    hal_s32_t *last_error;
    hal_u32_t *sync_count;
    lsc_node_hal_t nodes[LSC_MAX_NODES];
} lsc_hal_t;

typedef struct {
    XML_Parser parser;
    lsc_config_t *config;
    lsc_node_config_t *current_node;
    lsc_pdo_config_t *current_pdo;
    lsc_pdo_direction_t current_direction;
    bool root_seen;
    bool failed;
    char error[256];
} lsc_parser_state_t;

typedef struct {
    char config_file[LSC_PATH_LEN];
    char component_name[HAL_NAME_LEN + 1];
    char interface_override[IFNAMSIZ];
    uint32_t period_override_us;
    bool has_interface_override;
    bool has_period_override;
} lsc_options_t;

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_microseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

static void sleep_microseconds(uint32_t delay_us)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(delay_us / 1000000U);
    delay.tv_nsec = (long)(delay_us % 1000000U) * 1000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stop_requested) {
    }
}

static int copy_text(char *destination, size_t destination_size, const char *source)
{
    size_t source_length = strlen(source);

    if (source_length == 0U || source_length >= destination_size) {
        return -1;
    }

    memcpy(destination, source, source_length + 1U);
    return 0;
}

static const char *attribute_value(const char **attributes, const char *name)
{
    unsigned int index;

    for (index = 0U; attributes[index] != NULL; index += 2U) {
        if (strcmp(attributes[index], name) == 0) {
            return attributes[index + 1U];
        }
    }

    return NULL;
}

static int parse_unsigned(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_boolean(const char *text, bool *value)
{
    if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *value = true;
        return 0;
    }

    if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *value = false;
        return 0;
    }

    return -1;
}

static int parse_real(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int parse_hal_type(const char *text, lsc_hal_type_t *hal_type)
{
    if (strcmp(text, "bit") == 0) {
        *hal_type = LSC_HAL_BIT;
    } else if (strcmp(text, "u32") == 0) {
        *hal_type = LSC_HAL_U32;
    } else if (strcmp(text, "s32") == 0) {
        *hal_type = LSC_HAL_S32;
    } else if (strcmp(text, "float") == 0) {
        *hal_type = LSC_HAL_FLOAT;
    } else {
        return -1;
    }

    return 0;
}

static bool valid_hal_segment(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (*cursor == '\0') {
        return false;
    }

    while (*cursor != '\0') {
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-')) {
            return false;
        }
        ++cursor;
    }

    return true;
}

static void parser_fail(lsc_parser_state_t *state, const char *format, ...)
{
    va_list arguments;

    if (state->failed) {
        return;
    }

    va_start(arguments, format);
    vsnprintf(state->error, sizeof(state->error), format, arguments);
    va_end(arguments);
    state->failed = true;
    XML_StopParser(state->parser, XML_FALSE);
}

static uint16_t default_pdo_cob_id(lsc_pdo_direction_t direction, uint8_t number, uint8_t node_id)
{
    static const uint16_t rpdo_bases[LSC_MAX_PDOS] = {0x200U, 0x300U, 0x400U, 0x500U};
    static const uint16_t tpdo_bases[LSC_MAX_PDOS] = {0x180U, 0x280U, 0x380U, 0x480U};

    return (uint16_t)((direction == LSC_PDO_RPDO ? rpdo_bases[number - 1U]
                                                 : tpdo_bases[number - 1U]) +
                      node_id);
}

static void parse_socketcan_element(lsc_parser_state_t *state, const char **attributes)
{
    const char *text;
    uint32_t value;

    if (state->root_seen || state->current_node != NULL) {
        parser_fail(state, "duplicate or misplaced socketcan element");
        return;
    }

    state->root_seen = true;
    text = attribute_value(attributes, "interface");
    if (text != NULL && copy_text(state->config->interface_name,
                                  sizeof(state->config->interface_name),
                                  text) != 0) {
        parser_fail(state, "invalid SocketCAN interface name");
        return;
    }

    text = attribute_value(attributes, "periodUs");
    if (text != NULL) {
        if (parse_unsigned(text, 100U, 1000000U, &value) != 0) {
            parser_fail(state, "invalid periodUs value");
            return;
        }
        state->config->period_us = value;
    }

    text = attribute_value(attributes, "syncPeriodUs");
    if (text != NULL) {
        if (parse_unsigned(text, 0U, 1000000U, &value) != 0 ||
            (value != 0U && value < 100U)) {
            parser_fail(state, "invalid syncPeriodUs value");
            return;
        }
        state->config->sync_period_us = value;
    }

    text = attribute_value(attributes, "sdoTimeoutMs");
    if (text != NULL) {
        if (parse_unsigned(text, 10U, 60000U, &value) != 0) {
            parser_fail(state, "invalid sdoTimeoutMs value");
            return;
        }
        state->config->sdo_timeout_ms = value;
    }
}

static void parse_node_element(lsc_parser_state_t *state, const char **attributes)
{
    lsc_node_config_t *node;
    const char *text;
    uint32_t value;
    unsigned int index;

    if (!state->root_seen || state->current_node != NULL || state->current_pdo != NULL) {
        parser_fail(state, "misplaced node element");
        return;
    }
    if (state->config->node_count >= LSC_MAX_NODES) {
        parser_fail(state, "too many CANopen nodes; maximum is %u", LSC_MAX_NODES);
        return;
    }

    node = &state->config->nodes[state->config->node_count];
    memset(node, 0, sizeof(*node));
    node->start_node = true;
    node->heartbeat_timeout_ms = 1000U;

    text = attribute_value(attributes, "name");
    if (text == NULL || copy_text(node->name, sizeof(node->name), text) != 0 ||
        !valid_hal_segment(node->name)) {
        parser_fail(state, "node has an invalid or missing name");
        return;
    }

    text = attribute_value(attributes, "id");
    if (text == NULL || parse_unsigned(text, 1U, 127U, &value) != 0) {
        parser_fail(state, "node %s has an invalid or missing id", node->name);
        return;
    }
    node->node_id = (uint8_t)value;

    for (index = 0U; index < state->config->node_count; ++index) {
        if (state->config->nodes[index].node_id == node->node_id) {
            parser_fail(state, "duplicate CANopen node id %u", node->node_id);
            return;
        }
        if (strcmp(state->config->nodes[index].name, node->name) == 0) {
            parser_fail(state, "duplicate CANopen node name %s", node->name);
            return;
        }
    }

    text = attribute_value(attributes, "configPdos");
    if (text != NULL && parse_boolean(text, &node->config_pdos) != 0) {
        parser_fail(state, "node %s has an invalid configPdos value", node->name);
        return;
    }

    text = attribute_value(attributes, "startNode");
    if (text != NULL && parse_boolean(text, &node->start_node) != 0) {
        parser_fail(state, "node %s has an invalid startNode value", node->name);
        return;
    }

    text = attribute_value(attributes, "heartbeatProducerMs");
    if (text != NULL) {
        if (parse_unsigned(text, 0U, UINT16_MAX, &value) != 0) {
            parser_fail(state, "node %s has an invalid heartbeatProducerMs value", node->name);
            return;
        }
        node->heartbeat_producer_ms = (uint16_t)value;
    }

    text = attribute_value(attributes, "heartbeatTimeoutMs");
    if (text != NULL) {
        if (parse_unsigned(text, 0U, 600000U, &value) != 0) {
            parser_fail(state, "node %s has an invalid heartbeatTimeoutMs value", node->name);
            return;
        }
        node->heartbeat_timeout_ms = value;
    }

    state->config->node_count += 1U;
    state->current_node = node;
}

static void parse_pdo_element(lsc_parser_state_t *state,
                              lsc_pdo_direction_t direction,
                              const char **attributes)
{
    lsc_pdo_config_t *pdo;
    const char *text;
    uint32_t value;

    if (state->current_node == NULL || state->current_pdo != NULL) {
        parser_fail(state, "misplaced PDO element");
        return;
    }

    text = attribute_value(attributes, "number");
    if (text == NULL || parse_unsigned(text, 1U, LSC_MAX_PDOS, &value) != 0) {
        parser_fail(state, "node %s has a PDO with an invalid or missing number",
                    state->current_node->name);
        return;
    }

    pdo = direction == LSC_PDO_RPDO ? &state->current_node->rpdos[value - 1U]
                                    : &state->current_node->tpdos[value - 1U];
    if (pdo->defined) {
        parser_fail(state, "node %s has duplicate %s number %u",
                    state->current_node->name,
                    direction == LSC_PDO_RPDO ? "RPDO" : "TPDO",
                    value);
        return;
    }

    memset(pdo, 0, sizeof(*pdo));
    pdo->defined = true;
    pdo->number = (uint8_t)value;
    pdo->cob_id = default_pdo_cob_id(direction, pdo->number, state->current_node->node_id);
    pdo->transmission_type = 255U;

    text = attribute_value(attributes, "cobId");
    if (text != NULL) {
        if (parse_unsigned(text, 1U, CAN_SFF_MASK, &value) != 0) {
            parser_fail(state, "node %s PDO %u has an invalid cobId",
                        state->current_node->name,
                        pdo->number);
            return;
        }
        pdo->cob_id = (uint16_t)value;
    }

    text = attribute_value(attributes, "transmissionType");
    if (text != NULL) {
        if (parse_unsigned(text, 0U, UINT8_MAX, &value) != 0) {
            parser_fail(state, "node %s PDO %u has an invalid transmissionType",
                        state->current_node->name,
                        pdo->number);
            return;
        }
        pdo->transmission_type = (uint8_t)value;
    }

    text = attribute_value(attributes, "periodMs");
    if (text != NULL) {
        if (direction != LSC_PDO_RPDO || parse_unsigned(text, 0U, 600000U, &value) != 0) {
            parser_fail(state, "node %s PDO %u has an invalid periodMs",
                        state->current_node->name,
                        pdo->number);
            return;
        }
        pdo->period_ms = value;
    }

    text = attribute_value(attributes, "eventTimerMs");
    if (text != NULL) {
        if (direction != LSC_PDO_TPDO || parse_unsigned(text, 0U, UINT16_MAX, &value) != 0) {
            parser_fail(state, "node %s PDO %u has an invalid eventTimerMs",
                        state->current_node->name,
                        pdo->number);
            return;
        }
        pdo->event_timer_ms = (uint16_t)value;
        pdo->event_timer_set = true;
    }

    state->current_pdo = pdo;
    state->current_direction = direction;
}

static void parse_entry_element(lsc_parser_state_t *state, const char **attributes)
{
    lsc_entry_config_t *entry;
    const char *text;
    uint32_t value;
    unsigned int bit_offset = 0U;
    unsigned int index;

    if (state->current_pdo == NULL || state->current_node == NULL) {
        parser_fail(state, "misplaced pdoEntry element");
        return;
    }
    if (state->current_pdo->entry_count >= LSC_MAX_ENTRIES) {
        parser_fail(state, "node %s PDO %u has too many entries; maximum is %u",
                    state->current_node->name,
                    state->current_pdo->number,
                    LSC_MAX_ENTRIES);
        return;
    }

    for (index = 0U; index < state->current_pdo->entry_count; ++index) {
        bit_offset += state->current_pdo->entries[index].bit_length;
    }

    entry = &state->current_pdo->entries[state->current_pdo->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->scale = 1.0;

    text = attribute_value(attributes, "name");
    if (text == NULL || copy_text(entry->name, sizeof(entry->name), text) != 0 ||
        !valid_hal_segment(entry->name)) {
        parser_fail(state, "node %s PDO %u has an invalid or missing entry name",
                    state->current_node->name,
                    state->current_pdo->number);
        return;
    }

    for (index = 0U; index < state->current_pdo->entry_count; ++index) {
        if (strcmp(state->current_pdo->entries[index].name, entry->name) == 0) {
            parser_fail(state, "node %s PDO %u has duplicate entry name %s",
                        state->current_node->name,
                        state->current_pdo->number,
                        entry->name);
            return;
        }
    }

    text = attribute_value(attributes, "index");
    if (text == NULL || parse_unsigned(text, 0U, UINT16_MAX, &value) != 0) {
        parser_fail(state, "entry %s has an invalid or missing index", entry->name);
        return;
    }
    entry->index = (uint16_t)value;

    text = attribute_value(attributes, "subIdx");
    if (text == NULL || parse_unsigned(text, 0U, UINT8_MAX, &value) != 0) {
        parser_fail(state, "entry %s has an invalid or missing subIdx", entry->name);
        return;
    }
    entry->subindex = (uint8_t)value;

    text = attribute_value(attributes, "bitLen");
    if (text == NULL || parse_unsigned(text, 1U, 32U, &value) != 0) {
        parser_fail(state, "entry %s has an invalid or missing bitLen", entry->name);
        return;
    }
    entry->bit_length = (uint8_t)value;
    entry->bit_offset = (uint8_t)bit_offset;
    if (bit_offset + entry->bit_length > 64U) {
        parser_fail(state, "node %s PDO %u exceeds the Classical CAN 64-bit payload",
                    state->current_node->name,
                    state->current_pdo->number);
        return;
    }

    entry->hal_type = entry->bit_length == 1U ? LSC_HAL_BIT : LSC_HAL_U32;
    text = attribute_value(attributes, "halType");
    if (text != NULL && parse_hal_type(text, &entry->hal_type) != 0) {
        parser_fail(state, "entry %s has an invalid halType", entry->name);
        return;
    }
    if (entry->hal_type == LSC_HAL_BIT && entry->bit_length != 1U) {
        parser_fail(state, "entry %s uses halType=bit but bitLen is not 1", entry->name);
        return;
    }
    entry->raw_signed = entry->hal_type == LSC_HAL_S32;

    text = attribute_value(attributes, "signed");
    if (text != NULL) {
        if (entry->hal_type != LSC_HAL_FLOAT ||
            parse_boolean(text, &entry->raw_signed) != 0) {
            parser_fail(state,
                        "entry %s has an invalid signed value; it is only valid for halType=float",
                        entry->name);
            return;
        }
    }

    text = attribute_value(attributes, "scale");
    if (text != NULL && (parse_real(text, &entry->scale) != 0 || entry->scale == 0.0)) {
        parser_fail(state, "entry %s has an invalid scale", entry->name);
        return;
    }

    text = attribute_value(attributes, "offset");
    if (text != NULL && parse_real(text, &entry->offset) != 0) {
        parser_fail(state, "entry %s has an invalid offset", entry->name);
        return;
    }

    if (entry->hal_type != LSC_HAL_FLOAT &&
        (entry->scale != 1.0 || entry->offset != 0.0)) {
        parser_fail(state, "entry %s uses scale or offset without halType=float", entry->name);
        return;
    }

    state->current_pdo->entry_count += 1U;
    state->current_pdo->data_length =
        (uint8_t)((bit_offset + entry->bit_length + 7U) / 8U);
}

static void XMLCALL start_element(void *user_data, const char *name, const char **attributes)
{
    lsc_parser_state_t *state = user_data;

    if (state->failed) {
        return;
    }

    if (strcmp(name, "socketcan") == 0) {
        parse_socketcan_element(state, attributes);
    } else if (strcmp(name, "node") == 0) {
        parse_node_element(state, attributes);
    } else if (strcmp(name, "rpdo") == 0) {
        parse_pdo_element(state, LSC_PDO_RPDO, attributes);
    } else if (strcmp(name, "tpdo") == 0) {
        parse_pdo_element(state, LSC_PDO_TPDO, attributes);
    } else if (strcmp(name, "pdoEntry") == 0) {
        parse_entry_element(state, attributes);
    } else {
        parser_fail(state, "unsupported XML element %s", name);
    }
}

static void XMLCALL end_element(void *user_data, const char *name)
{
    lsc_parser_state_t *state = user_data;

    if (strcmp(name, "rpdo") == 0 || strcmp(name, "tpdo") == 0) {
        if (state->current_pdo != NULL && state->current_pdo->entry_count == 0U) {
            parser_fail(state, "node %s PDO %u has no pdoEntry elements",
                        state->current_node->name,
                        state->current_pdo->number);
            return;
        }
        state->current_pdo = NULL;
        state->current_direction = LSC_PDO_NONE;
    } else if (strcmp(name, "node") == 0) {
        state->current_node = NULL;
    }
}

static int validate_configuration(lsc_config_t *config, char *error, size_t error_size)
{
    unsigned int node_index;
    unsigned int other_node_index;
    unsigned int pdo_index;
    unsigned int other_pdo_index;
    bool has_pdo;

    if (config->node_count == 0U) {
        snprintf(error, error_size, "configuration contains no CANopen nodes");
        return -1;
    }

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        lsc_node_config_t *node = &config->nodes[node_index];
        has_pdo = false;

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            if (node->rpdos[pdo_index].defined || node->tpdos[pdo_index].defined) {
                has_pdo = true;
            }
            if (node->rpdos[pdo_index].defined && node->tpdos[pdo_index].defined &&
                node->rpdos[pdo_index].cob_id == node->tpdos[pdo_index].cob_id) {
                snprintf(error,
                         error_size,
                         "node %s has RPDO and TPDO COB-ID collision 0x%03X",
                         node->name,
                         node->rpdos[pdo_index].cob_id);
                return -1;
            }
        }

        if (!has_pdo) {
            snprintf(error, error_size, "node %s contains no PDO definitions", node->name);
            return -1;
        }

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            lsc_pdo_config_t *rpdo = &node->rpdos[pdo_index];
            lsc_pdo_config_t *tpdo = &node->tpdos[pdo_index];

            for (other_pdo_index = pdo_index + 1U;
                 other_pdo_index < LSC_MAX_PDOS;
                 ++other_pdo_index) {
                if (rpdo->defined && node->rpdos[other_pdo_index].defined &&
                    rpdo->cob_id == node->rpdos[other_pdo_index].cob_id) {
                    snprintf(error,
                             error_size,
                             "node %s has duplicate RPDO COB-ID 0x%03X",
                             node->name,
                             rpdo->cob_id);
                    return -1;
                }
                if (tpdo->defined && node->tpdos[other_pdo_index].defined &&
                    tpdo->cob_id == node->tpdos[other_pdo_index].cob_id) {
                    snprintf(error,
                             error_size,
                             "node %s has duplicate TPDO COB-ID 0x%03X",
                             node->name,
                             tpdo->cob_id);
                    return -1;
                }
            }
        }

        for (other_node_index = node_index + 1U;
             other_node_index < config->node_count;
             ++other_node_index) {
            lsc_node_config_t *other_node = &config->nodes[other_node_index];
            unsigned int left;
            unsigned int right;

            for (left = 0U; left < LSC_MAX_PDOS; ++left) {
                for (right = 0U; right < LSC_MAX_PDOS; ++right) {
                    if (node->tpdos[left].defined && other_node->tpdos[right].defined &&
                        node->tpdos[left].cob_id == other_node->tpdos[right].cob_id) {
                        snprintf(error,
                                 error_size,
                                 "nodes %s and %s share TPDO COB-ID 0x%03X",
                                 node->name,
                                 other_node->name,
                                 node->tpdos[left].cob_id);
                        return -1;
                    }
                    if (node->rpdos[left].defined && other_node->rpdos[right].defined &&
                        node->rpdos[left].cob_id == other_node->rpdos[right].cob_id) {
                        snprintf(error,
                                 error_size,
                                 "nodes %s and %s share RPDO COB-ID 0x%03X",
                                 node->name,
                                 other_node->name,
                                 node->rpdos[left].cob_id);
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}

static int load_configuration(const char *path, lsc_config_t *config, char *error, size_t error_size)
{
    lsc_parser_state_t state;
    XML_Parser parser;
    FILE *file;
    bool finished = false;

    memset(config, 0, sizeof(*config));
    copy_text(config->interface_name, sizeof(config->interface_name), LSC_DEFAULT_INTERFACE);
    config->period_us = LSC_DEFAULT_PERIOD_US;
    config->sdo_timeout_ms = LSC_DEFAULT_SDO_TIMEOUT_MS;

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    parser = XML_ParserCreate(NULL);
    if (parser == NULL) {
        fclose(file);
        snprintf(error, error_size, "cannot create Expat parser");
        return -1;
    }

    memset(&state, 0, sizeof(state));
    state.parser = parser;
    state.config = config;
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_element, end_element);

    while (!finished) {
        void *buffer = XML_GetBuffer(parser, LSC_XML_BUFFER_SIZE);
        size_t bytes_read;

        if (buffer == NULL) {
            snprintf(error, error_size, "Expat buffer allocation failed");
            XML_ParserFree(parser);
            fclose(file);
            return -1;
        }

        bytes_read = fread(buffer, 1U, LSC_XML_BUFFER_SIZE, file);
        if (ferror(file)) {
            snprintf(error, error_size, "error reading %s", path);
            XML_ParserFree(parser);
            fclose(file);
            return -1;
        }

        finished = feof(file) != 0;
        if (XML_ParseBuffer(parser, (int)bytes_read, finished) == XML_STATUS_ERROR) {
            if (state.failed) {
                snprintf(error,
                         error_size,
                         "%s:%lu: %s",
                         path,
                         XML_GetCurrentLineNumber(parser),
                         state.error);
            } else {
                snprintf(error,
                         error_size,
                         "%s:%lu: %s",
                         path,
                         XML_GetCurrentLineNumber(parser),
                         XML_ErrorString(XML_GetErrorCode(parser)));
            }
            XML_ParserFree(parser);
            fclose(file);
            return -1;
        }
    }

    XML_ParserFree(parser);
    fclose(file);

    if (!state.root_seen) {
        snprintf(error, error_size, "configuration has no socketcan root element");
        return -1;
    }

    return validate_configuration(config, error, error_size);
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s --config FILE [OPTIONS]\n"
            "  -c, --config FILE      CANopen XML mapping file\n"
            "  -i, --interface NAME   override XML SocketCAN interface\n"
            "  -p, --period-us USEC   override XML polling period\n"
            "  -n, --name NAME        HAL component prefix (default: %s)\n"
            "  -h, --help             show this help\n",
            program_name,
            LSC_DEFAULT_COMPONENT);
}

static int parse_options(int argc, char **argv, lsc_options_t *options)
{
    static const struct option long_options[] = {
        {"config", required_argument, NULL, 'c'},
        {"interface", required_argument, NULL, 'i'},
        {"period-us", required_argument, NULL, 'p'},
        {"name", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    uint32_t value;
    int option;

    memset(options, 0, sizeof(*options));
    if (copy_text(options->component_name,
                  sizeof(options->component_name),
                  LSC_DEFAULT_COMPONENT) != 0) {
        return -1;
    }

    while ((option = getopt_long(argc, argv, "c:i:p:n:h", long_options, NULL)) != -1) {
        switch (option) {
        case 'c':
            if (copy_text(options->config_file, sizeof(options->config_file), optarg) != 0) {
                fprintf(stderr, "invalid configuration path\n");
                return -1;
            }
            break;
        case 'i':
            if (copy_text(options->interface_override,
                          sizeof(options->interface_override),
                          optarg) != 0) {
                fprintf(stderr, "invalid SocketCAN interface name\n");
                return -1;
            }
            options->has_interface_override = true;
            break;
        case 'p':
            if (parse_unsigned(optarg, 100U, 1000000U, &value) != 0) {
                fprintf(stderr, "invalid polling period\n");
                return -1;
            }
            options->period_override_us = value;
            options->has_period_override = true;
            break;
        case 'n':
            if (copy_text(options->component_name,
                          sizeof(options->component_name),
                          optarg) != 0) {
                fprintf(stderr, "invalid HAL component name\n");
                return -1;
            }
            break;
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            return -1;
        }
    }

    if (options->config_file[0] == '\0') {
        fprintf(stderr, "--config is required\n");
        return -1;
    }
    if (optind != argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    return 0;
}

static int fail_socket(int socket_fd)
{
    int error_number = errno;

    close(socket_fd);
    errno = error_number;
    return -1;
}

static int open_can_socket(const char *interface_name)
{
    struct sockaddr_can address;
    struct ifreq interface_request;
    can_err_mask_t error_mask = CAN_ERR_MASK;
    int socket_fd;
    int flags;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        return -1;
    }

    flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return fail_socket(socket_fd);
    }

    if (setsockopt(socket_fd,
                   SOL_CAN_RAW,
                   CAN_RAW_ERR_FILTER,
                   &error_mask,
                   sizeof(error_mask)) < 0) {
        return fail_socket(socket_fd);
    }

    memset(&interface_request, 0, sizeof(interface_request));
    memcpy(interface_request.ifr_name, interface_name, strlen(interface_name) + 1U);
    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        return fail_socket(socket_fd);
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = interface_request.ifr_ifindex;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        return fail_socket(socket_fd);
    }

    return socket_fd;
}

static int write_can_frame(int socket_fd, const struct can_frame *frame, uint32_t timeout_ms)
{
    uint64_t deadline = monotonic_microseconds() + (uint64_t)timeout_ms * 1000U;

    while (!stop_requested) {
        ssize_t bytes_written = write(socket_fd, frame, sizeof(*frame));

        if (bytes_written == (ssize_t)sizeof(*frame)) {
            return 0;
        }
        if (bytes_written >= 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        for (;;) {
            struct pollfd descriptor;
            uint64_t now = monotonic_microseconds();
            int remaining_ms;
            int poll_result;

            if (now >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }

            remaining_ms = (int)((deadline - now + 999U) / 1000U);
            descriptor.fd = socket_fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1U, remaining_ms);
            if (poll_result > 0) {
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    errno = EIO;
                    return -1;
                }
                break;
            }
            if (poll_result == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (errno != EINTR) {
                return -1;
            }
        }
    }

    errno = EINTR;
    return -1;
}

static int send_can_data(int socket_fd,
                         uint16_t can_id,
                         const uint8_t *data,
                         uint8_t length,
                         uint32_t timeout_ms)
{
    struct can_frame frame;

    memset(&frame, 0, sizeof(frame));
    frame.can_id = can_id;
    frame.len = length;
    if (length > 0U) {
        memcpy(frame.data, data, length);
    }

    return write_can_frame(socket_fd, &frame, timeout_ms);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static int wait_for_sdo_response(int socket_fd,
                                 uint8_t node_id,
                                 uint16_t index,
                                 uint8_t subindex,
                                 uint32_t timeout_ms,
                                 uint32_t *abort_code)
{
    uint64_t deadline = monotonic_microseconds() + (uint64_t)timeout_ms * 1000U;

    while (!stop_requested) {
        struct pollfd descriptor;
        uint64_t now = monotonic_microseconds();
        int remaining_ms;
        int poll_result;

        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        remaining_ms = (int)((deadline - now + 999U) / 1000U);
        descriptor.fd = socket_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1U, remaining_ms);
        if (poll_result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            errno = EIO;
            return -1;
        }

        for (;;) {
            struct can_frame frame;
            ssize_t bytes_read = read(socket_fd, &frame, sizeof(frame));

            if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (bytes_read < 0 && errno == EINTR) {
                continue;
            }
            if (bytes_read != (ssize_t)sizeof(frame)) {
                if (bytes_read >= 0) {
                    errno = EMSGSIZE;
                }
                return -1;
            }
            if ((frame.can_id & CAN_ERR_FLAG) != 0U) {
                if ((frame.can_id & CAN_ERR_BUSOFF) != 0U) {
                    errno = ENETDOWN;
                    return -1;
                }
                continue;
            }
            if ((frame.can_id & CAN_SFF_MASK) != (uint16_t)(0x580U + node_id) ||
                frame.len < 4U) {
                continue;
            }
            if (frame.data[1] != (uint8_t)(index & 0xFFU) ||
                frame.data[2] != (uint8_t)(index >> 8U) ||
                frame.data[3] != subindex) {
                continue;
            }
            if (frame.data[0] == 0x60U) {
                return 0;
            }
            if (frame.data[0] == 0x80U && frame.len == 8U) {
                *abort_code = read_le32(&frame.data[4]);
                errno = ECANCELED;
                return -1;
            }

            errno = EPROTO;
            return -1;
        }
    }

    errno = EINTR;
    return -1;
}

static int sdo_download(int socket_fd,
                        uint8_t node_id,
                        uint16_t index,
                        uint8_t subindex,
                        uint32_t value,
                        uint8_t size,
                        uint32_t timeout_ms,
                        uint32_t *abort_code)
{
    uint8_t request[8] = {0U};

    if (size == 1U) {
        request[0] = 0x2FU;
    } else if (size == 2U) {
        request[0] = 0x2BU;
    } else if (size == 4U) {
        request[0] = 0x23U;
    } else {
        errno = EINVAL;
        return -1;
    }

    *abort_code = 0U;
    request[1] = (uint8_t)(index & 0xFFU);
    request[2] = (uint8_t)(index >> 8U);
    request[3] = subindex;
    request[4] = (uint8_t)(value & 0xFFU);
    request[5] = (uint8_t)((value >> 8U) & 0xFFU);
    request[6] = (uint8_t)((value >> 16U) & 0xFFU);
    request[7] = (uint8_t)((value >> 24U) & 0xFFU);

    if (send_can_data(socket_fd,
                      (uint16_t)(0x600U + node_id),
                      request,
                      sizeof(request),
                      timeout_ms) != 0) {
        return -1;
    }

    return wait_for_sdo_response(socket_fd,
                                 node_id,
                                 index,
                                 subindex,
                                 timeout_ms,
                                 abort_code);
}

static int send_nmt(int socket_fd, uint8_t command, uint8_t node_id, uint32_t timeout_ms)
{
    uint8_t payload[2] = {command, node_id};

    return send_can_data(socket_fd, 0U, payload, sizeof(payload), timeout_ms);
}

static int configure_pdo(int socket_fd,
                         uint8_t node_id,
                         lsc_pdo_direction_t direction,
                         const lsc_pdo_config_t *pdo,
                         uint32_t timeout_ms,
                         uint32_t *abort_code)
{
    uint16_t communication_index;
    uint16_t mapping_index;
    uint32_t disabled_cob_id = (uint32_t)pdo->cob_id | 0x80000000U;
    unsigned int entry_index;

    if (direction == LSC_PDO_RPDO) {
        communication_index = (uint16_t)(0x1400U + pdo->number - 1U);
        mapping_index = (uint16_t)(0x1600U + pdo->number - 1U);
    } else {
        communication_index = (uint16_t)(0x1800U + pdo->number - 1U);
        mapping_index = (uint16_t)(0x1A00U + pdo->number - 1U);
    }

    if (sdo_download(socket_fd,
                     node_id,
                     communication_index,
                     1U,
                     disabled_cob_id,
                     4U,
                     timeout_ms,
                     abort_code) != 0 ||
        sdo_download(socket_fd,
                     node_id,
                     mapping_index,
                     0U,
                     0U,
                     1U,
                     timeout_ms,
                     abort_code) != 0) {
        return -1;
    }

    for (entry_index = 0U; entry_index < pdo->entry_count; ++entry_index) {
        const lsc_entry_config_t *entry = &pdo->entries[entry_index];
        uint32_t mapping_value = ((uint32_t)entry->index << 16U) |
                                 ((uint32_t)entry->subindex << 8U) |
                                 entry->bit_length;

        if (sdo_download(socket_fd,
                         node_id,
                         mapping_index,
                         (uint8_t)(entry_index + 1U),
                         mapping_value,
                         4U,
                         timeout_ms,
                         abort_code) != 0) {
            return -1;
        }
    }

    if (sdo_download(socket_fd,
                     node_id,
                     mapping_index,
                     0U,
                     pdo->entry_count,
                     1U,
                     timeout_ms,
                     abort_code) != 0 ||
        sdo_download(socket_fd,
                     node_id,
                     communication_index,
                     2U,
                     pdo->transmission_type,
                     1U,
                     timeout_ms,
                     abort_code) != 0) {
        return -1;
    }

    if (direction == LSC_PDO_TPDO && pdo->event_timer_set &&
        sdo_download(socket_fd,
                     node_id,
                     communication_index,
                     5U,
                     pdo->event_timer_ms,
                     2U,
                     timeout_ms,
                     abort_code) != 0) {
        return -1;
    }

    return sdo_download(socket_fd,
                        node_id,
                        communication_index,
                        1U,
                        pdo->cob_id,
                        4U,
                        timeout_ms,
                        abort_code);
}

static int configure_node(int socket_fd,
                          const lsc_config_t *config,
                          const lsc_node_config_t *node,
                          lsc_node_hal_t *node_hal)
{
    unsigned int pdo_index;
    uint32_t abort_code = 0U;

    *node_hal->config_ok = 0;
    *node_hal->sdo_abort = 0U;

    if (node->config_pdos) {
        if (send_nmt(socket_fd, 0x80U, node->node_id, config->sdo_timeout_ms) != 0) {
            return -1;
        }
        sleep_microseconds(20000U);

        if (node->heartbeat_producer_ms > 0U &&
            sdo_download(socket_fd,
                         node->node_id,
                         0x1017U,
                         0U,
                         node->heartbeat_producer_ms,
                         2U,
                         config->sdo_timeout_ms,
                         &abort_code) != 0) {
            *node_hal->sdo_abort = abort_code;
            return -1;
        }

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            if (node->rpdos[pdo_index].defined &&
                configure_pdo(socket_fd,
                              node->node_id,
                              LSC_PDO_RPDO,
                              &node->rpdos[pdo_index],
                              config->sdo_timeout_ms,
                              &abort_code) != 0) {
                *node_hal->sdo_abort = abort_code;
                return -1;
            }
            if (node->tpdos[pdo_index].defined &&
                configure_pdo(socket_fd,
                              node->node_id,
                              LSC_PDO_TPDO,
                              &node->tpdos[pdo_index],
                              config->sdo_timeout_ms,
                              &abort_code) != 0) {
                *node_hal->sdo_abort = abort_code;
                return -1;
            }
        }
    }

    if (node->start_node &&
        send_nmt(socket_fd, 0x01U, node->node_id, config->sdo_timeout_ms) != 0) {
        return -1;
    }

    *node_hal->config_ok = 1;
    return 0;
}

static int configure_network(int socket_fd, const lsc_config_t *config, lsc_hal_t *hal_data)
{
    unsigned int node_index;

    *hal_data->config_ok = 0;
    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        if (configure_node(socket_fd,
                           config,
                           &config->nodes[node_index],
                           &hal_data->nodes[node_index]) != 0) {
            return -1;
        }
    }

    *hal_data->config_ok = 1;
    return 0;
}

static int export_entry_pin(int component_id,
                            const char *pin_name,
                            hal_pin_dir_t direction,
                            lsc_hal_type_t hal_type,
                            lsc_entry_hal_t *entry_hal)
{
    switch (hal_type) {
    case LSC_HAL_BIT:
        return hal_pin_bit_newf(direction, &entry_hal->value.bit, component_id, "%s", pin_name);
    case LSC_HAL_U32:
        return hal_pin_u32_newf(direction, &entry_hal->value.u32, component_id, "%s", pin_name);
    case LSC_HAL_S32:
        return hal_pin_s32_newf(direction, &entry_hal->value.s32, component_id, "%s", pin_name);
    case LSC_HAL_FLOAT:
        return hal_pin_float_newf(direction, &entry_hal->value.real, component_id, "%s", pin_name);
    }

    return -EINVAL;
}

static int export_hal_pins(int component_id,
                           const char *component_name,
                           const lsc_config_t *config,
                           lsc_hal_t *hal_data)
{
    unsigned int node_index;
    int result;

    result = hal_pin_bit_newf(HAL_OUT,
                              &hal_data->connected,
                              component_id,
                              "%s.connected",
                              component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT,
                              &hal_data->config_ok,
                              component_id,
                              "%s.config-ok",
                              component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT,
                              &hal_data->bus_off,
                              component_id,
                              "%s.bus-off",
                              component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT,
                              &hal_data->error_count,
                              component_id,
                              "%s.error-count",
                              component_name);
    if (result < 0) return result;
    result = hal_pin_s32_newf(HAL_OUT,
                              &hal_data->last_error,
                              component_id,
                              "%s.last-error",
                              component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT,
                              &hal_data->sync_count,
                              component_id,
                              "%s.sync-count",
                              component_name);
    if (result < 0) return result;

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        unsigned int pdo_index;

        result = hal_pin_bit_newf(HAL_IN,
                                  &node_hal->enable,
                                  component_id,
                                  "%s.%s.enable",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_bit_newf(HAL_OUT,
                                  &node_hal->online,
                                  component_id,
                                  "%s.%s.online",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_bit_newf(HAL_OUT,
                                  &node_hal->operational,
                                  component_id,
                                  "%s.%s.operational",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_bit_newf(HAL_OUT,
                                  &node_hal->config_ok,
                                  component_id,
                                  "%s.%s.config-ok",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_u32_newf(HAL_OUT,
                                  &node_hal->nmt_state,
                                  component_id,
                                  "%s.%s.nmt-state",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_u32_newf(HAL_OUT,
                                  &node_hal->heartbeat_age_ms,
                                  component_id,
                                  "%s.%s.heartbeat-age-ms",
                                  component_name,
                                  node->name);
        if (result < 0) return result;
        result = hal_pin_u32_newf(HAL_OUT,
                                  &node_hal->sdo_abort,
                                  component_id,
                                  "%s.%s.sdo-abort",
                                  component_name,
                                  node->name);
        if (result < 0) return result;

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            const lsc_pdo_config_t *rpdo = &node->rpdos[pdo_index];
            const lsc_pdo_config_t *tpdo = &node->tpdos[pdo_index];
            unsigned int entry_index;

            if (rpdo->defined) {
                lsc_rpdo_hal_t *rpdo_hal = &node_hal->rpdos[pdo_index];

                result = hal_pin_bit_newf(HAL_IN,
                                          &rpdo_hal->send,
                                          component_id,
                                          "%s.%s.rpdo-%u.send",
                                          component_name,
                                          node->name,
                                          rpdo->number);
                if (result < 0) return result;
                result = hal_pin_u32_newf(HAL_OUT,
                                          &rpdo_hal->tx_count,
                                          component_id,
                                          "%s.%s.rpdo-%u.tx-count",
                                          component_name,
                                          node->name,
                                          rpdo->number);
                if (result < 0) return result;
                result = hal_pin_s32_newf(HAL_OUT,
                                          &rpdo_hal->last_error,
                                          component_id,
                                          "%s.%s.rpdo-%u.last-error",
                                          component_name,
                                          node->name,
                                          rpdo->number);
                if (result < 0) return result;

                for (entry_index = 0U; entry_index < rpdo->entry_count; ++entry_index) {
                    char pin_name[HAL_NAME_LEN + 1];

                    if (snprintf(pin_name,
                                 sizeof(pin_name),
                                 "%s.%s.rpdo-%u.%s",
                                 component_name,
                                 node->name,
                                 rpdo->number,
                                 rpdo->entries[entry_index].name) >= (int)sizeof(pin_name)) {
                        return -ENAMETOOLONG;
                    }
                    result = export_entry_pin(component_id,
                                              pin_name,
                                              HAL_IN,
                                              rpdo->entries[entry_index].hal_type,
                                              &rpdo_hal->entries[entry_index]);
                    if (result < 0) return result;
                }
            }

            if (tpdo->defined) {
                lsc_tpdo_hal_t *tpdo_hal = &node_hal->tpdos[pdo_index];

                result = hal_pin_u32_newf(HAL_OUT,
                                          &tpdo_hal->rx_count,
                                          component_id,
                                          "%s.%s.tpdo-%u.rx-count",
                                          component_name,
                                          node->name,
                                          tpdo->number);
                if (result < 0) return result;
                result = hal_pin_s32_newf(HAL_OUT,
                                          &tpdo_hal->last_error,
                                          component_id,
                                          "%s.%s.tpdo-%u.last-error",
                                          component_name,
                                          node->name,
                                          tpdo->number);
                if (result < 0) return result;

                for (entry_index = 0U; entry_index < tpdo->entry_count; ++entry_index) {
                    char pin_name[HAL_NAME_LEN + 1];

                    if (snprintf(pin_name,
                                 sizeof(pin_name),
                                 "%s.%s.tpdo-%u.%s",
                                 component_name,
                                 node->name,
                                 tpdo->number,
                                 tpdo->entries[entry_index].name) >= (int)sizeof(pin_name)) {
                        return -ENAMETOOLONG;
                    }
                    result = export_entry_pin(component_id,
                                              pin_name,
                                              HAL_OUT,
                                              tpdo->entries[entry_index].hal_type,
                                              &tpdo_hal->entries[entry_index]);
                    if (result < 0) return result;
                }
            }
        }
    }

    return 0;
}

static void initialize_hal_pins(const lsc_config_t *config, lsc_hal_t *hal_data)
{
    unsigned int node_index;

    *hal_data->connected = 0;
    *hal_data->config_ok = 0;
    *hal_data->bus_off = 0;
    *hal_data->error_count = 0U;
    *hal_data->last_error = 0;
    *hal_data->sync_count = 0U;

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        unsigned int pdo_index;

        *node_hal->enable = 1;
        *node_hal->online = 0;
        *node_hal->operational = 0;
        *node_hal->config_ok = 0;
        *node_hal->nmt_state = 0U;
        *node_hal->heartbeat_age_ms = UINT32_MAX;
        *node_hal->sdo_abort = 0U;
        node_hal->last_heartbeat_us = 0U;

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            const lsc_pdo_config_t *rpdo = &node->rpdos[pdo_index];
            const lsc_pdo_config_t *tpdo = &node->tpdos[pdo_index];
            unsigned int entry_index;

            if (rpdo->defined) {
                lsc_rpdo_hal_t *rpdo_hal = &node_hal->rpdos[pdo_index];
                *rpdo_hal->send = 0;
                *rpdo_hal->tx_count = 0U;
                *rpdo_hal->last_error = 0;

                for (entry_index = 0U; entry_index < rpdo->entry_count; ++entry_index) {
                    switch (rpdo->entries[entry_index].hal_type) {
                    case LSC_HAL_BIT:
                        *rpdo_hal->entries[entry_index].value.bit = 0;
                        break;
                    case LSC_HAL_U32:
                        *rpdo_hal->entries[entry_index].value.u32 = 0U;
                        break;
                    case LSC_HAL_S32:
                        *rpdo_hal->entries[entry_index].value.s32 = 0;
                        break;
                    case LSC_HAL_FLOAT:
                        *rpdo_hal->entries[entry_index].value.real = 0.0;
                        break;
                    }
                }
            }

            if (tpdo->defined) {
                lsc_tpdo_hal_t *tpdo_hal = &node_hal->tpdos[pdo_index];
                *tpdo_hal->rx_count = 0U;
                *tpdo_hal->last_error = 0;

                for (entry_index = 0U; entry_index < tpdo->entry_count; ++entry_index) {
                    switch (tpdo->entries[entry_index].hal_type) {
                    case LSC_HAL_BIT:
                        *tpdo_hal->entries[entry_index].value.bit = 0;
                        break;
                    case LSC_HAL_U32:
                        *tpdo_hal->entries[entry_index].value.u32 = 0U;
                        break;
                    case LSC_HAL_S32:
                        *tpdo_hal->entries[entry_index].value.s32 = 0;
                        break;
                    case LSC_HAL_FLOAT:
                        *tpdo_hal->entries[entry_index].value.real = 0.0;
                        break;
                    }
                }
            }
        }
    }
}

static uint32_t bit_mask(uint8_t bit_length)
{
    return bit_length == 32U ? UINT32_MAX : ((uint32_t)1U << bit_length) - 1U;
}

static uint32_t extract_bits(const uint8_t *data, uint8_t bit_offset, uint8_t bit_length)
{
    uint32_t value = 0U;
    uint8_t bit_index;

    for (bit_index = 0U; bit_index < bit_length; ++bit_index) {
        uint8_t source_bit = (uint8_t)(bit_offset + bit_index);
        if ((data[source_bit / 8U] & (uint8_t)(1U << (source_bit % 8U))) != 0U) {
            value |= (uint32_t)1U << bit_index;
        }
    }

    return value;
}

static void insert_bits(uint8_t *data, uint8_t bit_offset, uint8_t bit_length, uint32_t value)
{
    uint8_t bit_index;

    for (bit_index = 0U; bit_index < bit_length; ++bit_index) {
        uint8_t destination_bit = (uint8_t)(bit_offset + bit_index);
        uint8_t mask = (uint8_t)(1U << (destination_bit % 8U));

        if ((value & ((uint32_t)1U << bit_index)) != 0U) {
            data[destination_bit / 8U] |= mask;
        } else {
            data[destination_bit / 8U] &= (uint8_t)~mask;
        }
    }
}

static int32_t sign_extend(uint32_t value, uint8_t bit_length)
{
    if (bit_length == 32U) {
        return (int32_t)value;
    }
    if ((value & ((uint32_t)1U << (bit_length - 1U))) != 0U) {
        value |= ~bit_mask(bit_length);
    }
    return (int32_t)value;
}

static void decode_entry(const lsc_entry_config_t *entry,
                         const uint8_t *data,
                         lsc_entry_hal_t *entry_hal)
{
    uint32_t raw_value = extract_bits(data, entry->bit_offset, entry->bit_length);

    switch (entry->hal_type) {
    case LSC_HAL_BIT:
        *entry_hal->value.bit = raw_value != 0U;
        break;
    case LSC_HAL_U32:
        *entry_hal->value.u32 = raw_value;
        break;
    case LSC_HAL_S32:
        *entry_hal->value.s32 = sign_extend(raw_value, entry->bit_length);
        break;
    case LSC_HAL_FLOAT:
        if (entry->raw_signed) {
            *entry_hal->value.real =
                (hal_float_t)sign_extend(raw_value, entry->bit_length) * entry->scale +
                entry->offset;
        } else {
            *entry_hal->value.real = (hal_float_t)raw_value * entry->scale + entry->offset;
        }
        break;
    }
}

static int encode_entry(const lsc_entry_config_t *entry,
                        const lsc_entry_hal_t *entry_hal,
                        uint8_t *data)
{
    uint32_t raw_value;
    int64_t signed_value;
    uint64_t unsigned_value;
    int64_t minimum;
    int64_t maximum;

    switch (entry->hal_type) {
    case LSC_HAL_BIT:
        raw_value = *entry_hal->value.bit != 0 ? 1U : 0U;
        break;
    case LSC_HAL_U32:
        raw_value = *entry_hal->value.u32;
        if ((raw_value & ~bit_mask(entry->bit_length)) != 0U) {
            errno = ERANGE;
            return -1;
        }
        break;
    case LSC_HAL_S32:
        signed_value = *entry_hal->value.s32;
        minimum = entry->bit_length == 32U ? INT32_MIN
                                           : -((int64_t)1 << (entry->bit_length - 1U));
        maximum = entry->bit_length == 32U ? INT32_MAX
                                           : ((int64_t)1 << (entry->bit_length - 1U)) - 1;
        if (signed_value < minimum || signed_value > maximum) {
            errno = ERANGE;
            return -1;
        }
        raw_value = (uint32_t)signed_value & bit_mask(entry->bit_length);
        break;
    case LSC_HAL_FLOAT:
        if (entry->raw_signed) {
            double scaled_value = (*entry_hal->value.real - entry->offset) / entry->scale;
            minimum = entry->bit_length == 32U ? INT32_MIN
                                               : -((int64_t)1 << (entry->bit_length - 1U));
            maximum = entry->bit_length == 32U ? INT32_MAX
                                               : ((int64_t)1 << (entry->bit_length - 1U)) - 1;
            if (!isfinite(scaled_value) || scaled_value < (double)minimum ||
                scaled_value > (double)maximum) {
                errno = ERANGE;
                return -1;
            }
            signed_value = llround(scaled_value);
            raw_value = (uint32_t)signed_value & bit_mask(entry->bit_length);
        } else {
            double scaled_value = (*entry_hal->value.real - entry->offset) / entry->scale;
            if (!isfinite(scaled_value) || scaled_value < 0.0 ||
                scaled_value > (double)bit_mask(entry->bit_length)) {
                errno = ERANGE;
                return -1;
            }
            unsigned_value = (uint64_t)llround(scaled_value);
            raw_value = (uint32_t)unsigned_value;
        }
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    insert_bits(data, entry->bit_offset, entry->bit_length, raw_value);
    return 0;
}

static void record_global_error(lsc_hal_t *hal_data, int error_number)
{
    *hal_data->last_error = (hal_s32_t)error_number;
    *hal_data->error_count += 1U;
}

static void process_tpdo(const lsc_pdo_config_t *pdo,
                         lsc_tpdo_hal_t *pdo_hal,
                         const struct can_frame *frame)
{
    unsigned int entry_index;

    if (frame->len < pdo->data_length) {
        *pdo_hal->last_error = EMSGSIZE;
        return;
    }

    for (entry_index = 0U; entry_index < pdo->entry_count; ++entry_index) {
        decode_entry(&pdo->entries[entry_index],
                     frame->data,
                     &pdo_hal->entries[entry_index]);
    }

    *pdo_hal->rx_count += 1U;
    *pdo_hal->last_error = 0;
}

static void process_received_frame(const lsc_config_t *config,
                                   lsc_hal_t *hal_data,
                                   const struct can_frame *frame,
                                   uint64_t now_us)
{
    uint16_t can_id;
    unsigned int node_index;

    if ((frame->can_id & CAN_ERR_FLAG) != 0U) {
        *hal_data->error_count += 1U;
        if ((frame->can_id & CAN_ERR_BUSOFF) != 0U) {
            *hal_data->bus_off = 1;
        }
        if ((frame->can_id & CAN_ERR_RESTARTED) != 0U) {
            *hal_data->bus_off = 0;
        }
        return;
    }
    if ((frame->can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) != 0U) {
        return;
    }

    can_id = (uint16_t)(frame->can_id & CAN_SFF_MASK);
    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        unsigned int pdo_index;

        if (can_id == (uint16_t)(0x700U + node->node_id) && frame->len >= 1U) {
            *node_hal->nmt_state = frame->data[0];
            *node_hal->operational = frame->data[0] == 0x05U;
            *node_hal->online = 1;
            *node_hal->heartbeat_age_ms = 0U;
            node_hal->last_heartbeat_us = now_us;
            return;
        }

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            const lsc_pdo_config_t *tpdo = &node->tpdos[pdo_index];

            if (tpdo->defined && can_id == tpdo->cob_id) {
                process_tpdo(tpdo, &node_hal->tpdos[pdo_index], frame);
                if (node->heartbeat_timeout_ms == 0U) {
                    *node_hal->online = 1;
                }
                *hal_data->bus_off = 0;
                return;
            }
        }
    }
}

static int receive_frames(int socket_fd,
                          const lsc_config_t *config,
                          lsc_hal_t *hal_data,
                          uint64_t now_us)
{
    unsigned int frame_index;

    for (frame_index = 0U; frame_index < LSC_MAX_RX_FRAMES; ++frame_index) {
        struct can_frame frame;
        ssize_t bytes_read = read(socket_fd, &frame, sizeof(frame));

        if (bytes_read == (ssize_t)sizeof(frame)) {
            process_received_frame(config, hal_data, &frame, now_us);
            continue;
        }
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read >= 0) {
            errno = EMSGSIZE;
        }
        return -1;
    }

    return 0;
}

static int send_rpdo(int socket_fd,
                     const lsc_pdo_config_t *pdo,
                     lsc_rpdo_hal_t *pdo_hal)
{
    struct can_frame frame;
    unsigned int entry_index;

    memset(&frame, 0, sizeof(frame));
    frame.can_id = pdo->cob_id;
    frame.len = pdo->data_length;

    for (entry_index = 0U; entry_index < pdo->entry_count; ++entry_index) {
        if (encode_entry(&pdo->entries[entry_index],
                         &pdo_hal->entries[entry_index],
                         frame.data) != 0) {
            *pdo_hal->last_error = errno;
            return -1;
        }
    }

    if (write_can_frame(socket_fd, &frame, LSC_PDO_WRITE_TIMEOUT_MS) != 0) {
        *pdo_hal->last_error = errno;
        return -1;
    }

    *pdo_hal->tx_count += 1U;
    *pdo_hal->last_error = 0;
    return 0;
}

static bool error_requires_reconnect(int error_number)
{
    return error_number == ENETDOWN || error_number == ENODEV ||
           error_number == ENXIO || error_number == ENETUNREACH ||
           error_number == EBADF || error_number == EIO;
}

static void prepare_configured_network(const lsc_config_t *config,
                                       lsc_hal_t *hal_data,
                                       uint64_t now_us)
{
    unsigned int node_index;

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        unsigned int pdo_index;

        node_hal->last_heartbeat_us = 0U;
        *node_hal->heartbeat_age_ms = UINT32_MAX;
        if (node->heartbeat_timeout_ms == 0U) {
            *node_hal->online = 1;
            *node_hal->operational = node->start_node;
            *node_hal->nmt_state = node->start_node ? 0x05U : 0x7FU;
        } else {
            *node_hal->online = 0;
            *node_hal->operational = 0;
            *node_hal->nmt_state = 0U;
        }

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            const lsc_pdo_config_t *pdo = &node->rpdos[pdo_index];
            lsc_rpdo_hal_t *pdo_hal = &node_hal->rpdos[pdo_index];

            if (!pdo->defined) {
                continue;
            }
            pdo_hal->previous_send = *pdo_hal->send != 0;
            pdo_hal->next_due_us =
                pdo->period_ms > 0U ? now_us + (uint64_t)pdo->period_ms * 1000U : 0U;
        }
    }
}

static void mark_network_down(int *socket_fd,
                              const lsc_config_t *config,
                              lsc_hal_t *hal_data)
{
    unsigned int node_index;

    if (*socket_fd >= 0) {
        close(*socket_fd);
        *socket_fd = -1;
    }

    *hal_data->connected = 0;
    *hal_data->config_ok = 0;
    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        *node_hal->online = 0;
        *node_hal->operational = 0;
        *node_hal->config_ok = 0;
    }
}

static int process_rpdo_transmissions(int socket_fd,
                                      const lsc_config_t *config,
                                      lsc_hal_t *hal_data,
                                      uint64_t now_us)
{
    unsigned int node_index;

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        unsigned int pdo_index;

        for (pdo_index = 0U; pdo_index < LSC_MAX_PDOS; ++pdo_index) {
            const lsc_pdo_config_t *pdo = &node->rpdos[pdo_index];
            lsc_rpdo_hal_t *pdo_hal = &node_hal->rpdos[pdo_index];
            bool send_requested;
            bool periodic_due;
            bool trigger;

            if (!pdo->defined) {
                continue;
            }

            trigger = *pdo_hal->send != 0;
            send_requested = trigger && !pdo_hal->previous_send;
            periodic_due = pdo->period_ms > 0U && now_us >= pdo_hal->next_due_us;
            pdo_hal->previous_send = trigger;

            if (*node_hal->enable == 0 || *node_hal->config_ok == 0 ||
                (!send_requested && !periodic_due)) {
                continue;
            }

            if (send_rpdo(socket_fd, pdo, pdo_hal) != 0) {
                int error_number = errno;
                record_global_error(hal_data, error_number);
                if (error_requires_reconnect(error_number)) {
                    return -1;
                }
            }

            if (periodic_due) {
                pdo_hal->next_due_us = now_us + (uint64_t)pdo->period_ms * 1000U;
            }
        }
    }

    return 0;
}

static int send_sync(int socket_fd, lsc_hal_t *hal_data)
{
    if (send_can_data(socket_fd, 0x080U, NULL, 0U, LSC_PDO_WRITE_TIMEOUT_MS) != 0) {
        return -1;
    }

    *hal_data->sync_count += 1U;
    return 0;
}

static void update_heartbeat_status(const lsc_config_t *config,
                                    lsc_hal_t *hal_data,
                                    uint64_t now_us)
{
    unsigned int node_index;

    for (node_index = 0U; node_index < config->node_count; ++node_index) {
        const lsc_node_config_t *node = &config->nodes[node_index];
        lsc_node_hal_t *node_hal = &hal_data->nodes[node_index];
        uint64_t age_ms;

        if (node->heartbeat_timeout_ms == 0U) {
            continue;
        }
        if (node_hal->last_heartbeat_us == 0U) {
            *node_hal->heartbeat_age_ms = UINT32_MAX;
            *node_hal->online = 0;
            *node_hal->operational = 0;
            continue;
        }

        age_ms = (now_us - node_hal->last_heartbeat_us) / 1000U;
        *node_hal->heartbeat_age_ms =
            age_ms > UINT32_MAX ? UINT32_MAX : (hal_u32_t)age_ms;
        if (age_ms > node->heartbeat_timeout_ms) {
            *node_hal->online = 0;
            *node_hal->operational = 0;
        }
    }
}

static void run_loop(const lsc_config_t *config, lsc_hal_t *hal_data)
{
    uint64_t reconnect_after_us = 0U;
    uint64_t next_sync_us = 0U;
    int socket_fd = -1;

    while (!stop_requested) {
        uint64_t now_us = monotonic_microseconds();

        if (socket_fd < 0 && now_us >= reconnect_after_us) {
            socket_fd = open_can_socket(config->interface_name);
            if (socket_fd < 0) {
                record_global_error(hal_data, errno);
                reconnect_after_us = now_us + (uint64_t)LSC_RECONNECT_DELAY_MS * 1000U;
            } else {
                *hal_data->connected = 1;
                *hal_data->bus_off = 0;
                if (configure_network(socket_fd, config, hal_data) != 0) {
                    record_global_error(hal_data, errno);
                    mark_network_down(&socket_fd, config, hal_data);
                    reconnect_after_us = now_us +
                                         (uint64_t)LSC_RECONNECT_DELAY_MS * 1000U;
                } else {
                    *hal_data->last_error = 0;
                    prepare_configured_network(config, hal_data, now_us);
                    next_sync_us = config->sync_period_us > 0U
                                       ? now_us + config->sync_period_us
                                       : 0U;
                }
            }
        }

        if (socket_fd >= 0) {
            if (receive_frames(socket_fd, config, hal_data, now_us) != 0) {
                record_global_error(hal_data, errno);
                mark_network_down(&socket_fd, config, hal_data);
                reconnect_after_us = now_us + (uint64_t)LSC_RECONNECT_DELAY_MS * 1000U;
            } else if (process_rpdo_transmissions(socket_fd,
                                                  config,
                                                  hal_data,
                                                  now_us) != 0) {
                mark_network_down(&socket_fd, config, hal_data);
                reconnect_after_us = now_us + (uint64_t)LSC_RECONNECT_DELAY_MS * 1000U;
            } else if (config->sync_period_us > 0U && now_us >= next_sync_us) {
                if (send_sync(socket_fd, hal_data) != 0) {
                    record_global_error(hal_data, errno);
                    mark_network_down(&socket_fd, config, hal_data);
                    reconnect_after_us = now_us +
                                         (uint64_t)LSC_RECONNECT_DELAY_MS * 1000U;
                } else {
                    next_sync_us = now_us + config->sync_period_us;
                }
            }
        }

        update_heartbeat_status(config, hal_data, now_us);
        sleep_microseconds(config->period_us);
    }

    mark_network_down(&socket_fd, config, hal_data);
}

int main(int argc, char **argv)
{
    lsc_options_t options;
    lsc_config_t config;
    lsc_hal_t *hal_data;
    char error[512];
    int component_id;
    int result;

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (load_configuration(options.config_file, &config, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    if (options.has_interface_override &&
        copy_text(config.interface_name,
                  sizeof(config.interface_name),
                  options.interface_override) != 0) {
        fprintf(stderr, "invalid interface override\n");
        return EXIT_FAILURE;
    }
    if (options.has_period_override) {
        config.period_us = options.period_override_us;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    component_id = hal_init(options.component_name);
    if (component_id < 0) {
        fprintf(stderr, "hal_init(%s) failed: %d\n", options.component_name, component_id);
        return EXIT_FAILURE;
    }

    hal_data = hal_malloc(sizeof(*hal_data));
    if (hal_data == NULL) {
        fprintf(stderr, "hal_malloc failed\n");
        hal_exit(component_id);
        return EXIT_FAILURE;
    }
    memset(hal_data, 0, sizeof(*hal_data));

    result = export_hal_pins(component_id, options.component_name, &config, hal_data);
    if (result < 0) {
        fprintf(stderr, "failed to export HAL pins: %d\n", result);
        hal_exit(component_id);
        return EXIT_FAILURE;
    }
    initialize_hal_pins(&config, hal_data);

    result = hal_ready(component_id);
    if (result < 0) {
        fprintf(stderr, "hal_ready failed: %d\n", result);
        hal_exit(component_id);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "%s: config=%s interface=%s period=%u us nodes=%u\n",
            options.component_name,
            options.config_file,
            config.interface_name,
            config.period_us,
            config.node_count);
    run_loop(&config, hal_data);
    hal_exit(component_id);
    return EXIT_SUCCESS;
}
