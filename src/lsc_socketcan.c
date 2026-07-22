#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <rtapi.h>
#include <hal.h>

#define DEFAULT_INTERFACE "can0"
#define DEFAULT_COMPONENT_NAME "lsc_socketcan"
#define DEFAULT_PERIOD_US 1000U
#define RECONNECT_DELAY_MS 1000U
#define MAX_FRAMES_PER_CYCLE 256U

typedef struct {
    hal_bit_t *enable;
    hal_bit_t *connected;
    hal_bit_t *bus_off;
    hal_u32_t *error_count;
    hal_s32_t *last_error;
    hal_u32_t *last_can_error;

    hal_bit_t *tx_trigger;
    hal_u32_t *tx_id;
    hal_bit_t *tx_extended;
    hal_bit_t *tx_rtr;
    hal_u32_t *tx_length;
    hal_u32_t *tx_data[CAN_MAX_DLEN];
    hal_u32_t *tx_count;

    hal_bit_t *rx_new;
    hal_u32_t *rx_sequence;
    hal_u32_t *rx_id;
    hal_bit_t *rx_extended;
    hal_bit_t *rx_rtr;
    hal_u32_t *rx_length;
    hal_u32_t *rx_data[CAN_MAX_DLEN];
    hal_u32_t *rx_count;
} lsc_pins_t;

typedef struct {
    char interface_name[IFNAMSIZ];
    char component_name[HAL_NAME_LEN + 1];
    unsigned int period_us;
} lsc_options_t;

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  -i, --interface NAME  SocketCAN interface (default: %s)\n"
            "  -p, --period-us USEC  polling period (default: %u)\n"
            "  -n, --name NAME        HAL component name (default: %s)\n"
            "  -h, --help             show this help\n",
            program_name,
            DEFAULT_INTERFACE,
            DEFAULT_PERIOD_US,
            DEFAULT_COMPONENT_NAME);
}

static int copy_option(char *destination, size_t destination_size, const char *source)
{
    size_t source_length = strlen(source);

    if (source_length == 0U || source_length >= destination_size) {
        return -1;
    }

    memcpy(destination, source, source_length + 1U);
    return 0;
}

static int parse_period(const char *text, unsigned int *period_us)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 100UL || value > 1000000UL) {
        return -1;
    }

    *period_us = (unsigned int)value;
    return 0;
}

static int parse_options(int argc, char **argv, lsc_options_t *options)
{
    static const struct option long_options[] = {
        {"interface", required_argument, NULL, 'i'},
        {"period-us", required_argument, NULL, 'p'},
        {"name", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;

    if (copy_option(options->interface_name, sizeof(options->interface_name), DEFAULT_INTERFACE) != 0 ||
        copy_option(options->component_name, sizeof(options->component_name), DEFAULT_COMPONENT_NAME) != 0) {
        return -1;
    }
    options->period_us = DEFAULT_PERIOD_US;

    while ((option = getopt_long(argc, argv, "i:p:n:h", long_options, NULL)) != -1) {
        switch (option) {
        case 'i':
            if (copy_option(options->interface_name, sizeof(options->interface_name), optarg) != 0) {
                fprintf(stderr, "Invalid SocketCAN interface name: %s\n", optarg);
                return -1;
            }
            break;
        case 'p':
            if (parse_period(optarg, &options->period_us) != 0) {
                fprintf(stderr, "Invalid period: %s (expected 100..1000000 microseconds)\n", optarg);
                return -1;
            }
            break;
        case 'n':
            if (copy_option(options->component_name, sizeof(options->component_name), optarg) != 0) {
                fprintf(stderr, "Invalid HAL component name: %s\n", optarg);
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

    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    return 0;
}

static int export_pins(int component_id, const char *component_name, lsc_pins_t *pins)
{
    int result;
    unsigned int index;

    result = hal_pin_bit_newf(HAL_IN, &pins->enable, component_id, "%s.enable", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT, &pins->connected, component_id, "%s.connected", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT, &pins->bus_off, component_id, "%s.bus-off", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT, &pins->error_count, component_id, "%s.error-count", component_name);
    if (result < 0) return result;
    result = hal_pin_s32_newf(HAL_OUT, &pins->last_error, component_id, "%s.last-error", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT, &pins->last_can_error, component_id, "%s.last-can-error", component_name);
    if (result < 0) return result;

    result = hal_pin_bit_newf(HAL_IN, &pins->tx_trigger, component_id, "%s.tx-trigger", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_IN, &pins->tx_id, component_id, "%s.tx-id", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_IN, &pins->tx_extended, component_id, "%s.tx-extended", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_IN, &pins->tx_rtr, component_id, "%s.tx-rtr", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_IN, &pins->tx_length, component_id, "%s.tx-length", component_name);
    if (result < 0) return result;
    for (index = 0U; index < CAN_MAX_DLEN; ++index) {
        result = hal_pin_u32_newf(HAL_IN,
                                  &pins->tx_data[index],
                                  component_id,
                                  "%s.tx-data-%u",
                                  component_name,
                                  index);
        if (result < 0) return result;
    }
    result = hal_pin_u32_newf(HAL_OUT, &pins->tx_count, component_id, "%s.tx-count", component_name);
    if (result < 0) return result;

    result = hal_pin_bit_newf(HAL_OUT, &pins->rx_new, component_id, "%s.rx-new", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT, &pins->rx_sequence, component_id, "%s.rx-sequence", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT, &pins->rx_id, component_id, "%s.rx-id", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT, &pins->rx_extended, component_id, "%s.rx-extended", component_name);
    if (result < 0) return result;
    result = hal_pin_bit_newf(HAL_OUT, &pins->rx_rtr, component_id, "%s.rx-rtr", component_name);
    if (result < 0) return result;
    result = hal_pin_u32_newf(HAL_OUT, &pins->rx_length, component_id, "%s.rx-length", component_name);
    if (result < 0) return result;
    for (index = 0U; index < CAN_MAX_DLEN; ++index) {
        result = hal_pin_u32_newf(HAL_OUT,
                                  &pins->rx_data[index],
                                  component_id,
                                  "%s.rx-data-%u",
                                  component_name,
                                  index);
        if (result < 0) return result;
    }
    result = hal_pin_u32_newf(HAL_OUT, &pins->rx_count, component_id, "%s.rx-count", component_name);
    if (result < 0) return result;

    return 0;
}

static void initialize_pins(lsc_pins_t *pins)
{
    unsigned int index;

    *pins->enable = 1;
    *pins->connected = 0;
    *pins->bus_off = 0;
    *pins->error_count = 0U;
    *pins->last_error = 0;
    *pins->last_can_error = 0U;
    *pins->tx_trigger = 0;
    *pins->tx_id = 0U;
    *pins->tx_extended = 0;
    *pins->tx_rtr = 0;
    *pins->tx_length = 0U;
    *pins->tx_count = 0U;
    *pins->rx_new = 0;
    *pins->rx_sequence = 0U;
    *pins->rx_id = 0U;
    *pins->rx_extended = 0;
    *pins->rx_rtr = 0;
    *pins->rx_length = 0U;
    *pins->rx_count = 0U;

    for (index = 0U; index < CAN_MAX_DLEN; ++index) {
        *pins->tx_data[index] = 0U;
        *pins->rx_data[index] = 0U;
    }
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
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

static void record_system_error(lsc_pins_t *pins, int error_number)
{
    *pins->last_error = (hal_s32_t)error_number;
    *pins->error_count += 1U;
}

static void disconnect_socket(int *socket_fd, lsc_pins_t *pins)
{
    if (*socket_fd >= 0) {
        close(*socket_fd);
        *socket_fd = -1;
    }

    *pins->connected = 0;
}

static bool error_requires_reconnect(int error_number)
{
    return error_number == ENETDOWN || error_number == ENODEV || error_number == ENXIO ||
           error_number == ENETUNREACH || error_number == EBADF;
}

static void process_received_frame(const struct can_frame *frame, lsc_pins_t *pins)
{
    bool is_extended;
    unsigned int index;

    if ((frame->can_id & CAN_ERR_FLAG) != 0U) {
        *pins->last_can_error = frame->can_id & CAN_ERR_MASK;
        *pins->error_count += 1U;
        if ((frame->can_id & CAN_ERR_BUSOFF) != 0U) {
            *pins->bus_off = 1;
        }
        if ((frame->can_id & CAN_ERR_RESTARTED) != 0U) {
            *pins->bus_off = 0;
        }
        return;
    }

    is_extended = (frame->can_id & CAN_EFF_FLAG) != 0U;
    *pins->rx_id = frame->can_id & (is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    *pins->rx_extended = is_extended;
    *pins->rx_rtr = (frame->can_id & CAN_RTR_FLAG) != 0U;
    *pins->rx_length = frame->len;
    *pins->bus_off = 0;

    for (index = 0U; index < CAN_MAX_DLEN; ++index) {
        *pins->rx_data[index] = index < frame->len ? frame->data[index] : 0U;
    }

    *pins->rx_count += 1U;
    *pins->rx_sequence += 1U;
    *pins->rx_new = !*pins->rx_new;
}

static int receive_frames(int socket_fd, lsc_pins_t *pins)
{
    struct can_frame frame;
    unsigned int frame_count;

    for (frame_count = 0U; frame_count < MAX_FRAMES_PER_CYCLE; ++frame_count) {
        ssize_t bytes_read = read(socket_fd, &frame, sizeof(frame));

        if (bytes_read == (ssize_t)sizeof(frame)) {
            process_received_frame(&frame, pins);
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

static int build_transmit_frame(const lsc_pins_t *pins, struct can_frame *frame)
{
    bool is_extended = *pins->tx_extended != 0;
    hal_u32_t identifier = *pins->tx_id;
    hal_u32_t length = *pins->tx_length;
    unsigned int index;

    if (length > CAN_MAX_DLEN) {
        errno = EINVAL;
        return -1;
    }

    if ((!is_extended && identifier > CAN_SFF_MASK) || (is_extended && identifier > CAN_EFF_MASK)) {
        errno = EINVAL;
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->can_id = identifier;
    if (is_extended) {
        frame->can_id |= CAN_EFF_FLAG;
    }
    if (*pins->tx_rtr != 0) {
        frame->can_id |= CAN_RTR_FLAG;
    }
    frame->len = (uint8_t)length;

    for (index = 0U; index < length; ++index) {
        if (*pins->tx_data[index] > UINT8_MAX) {
            errno = EINVAL;
            return -1;
        }
        frame->data[index] = (uint8_t)*pins->tx_data[index];
    }

    return 0;
}

static int transmit_frame(int socket_fd, lsc_pins_t *pins)
{
    struct can_frame frame;
    ssize_t bytes_written;

    if (build_transmit_frame(pins, &frame) != 0) {
        return -1;
    }

    bytes_written = write(socket_fd, &frame, sizeof(frame));
    if (bytes_written != (ssize_t)sizeof(frame)) {
        if (bytes_written >= 0) {
            errno = EIO;
        }
        return -1;
    }

    *pins->tx_count += 1U;
    *pins->last_error = 0;
    return 0;
}

static void sleep_for_period(unsigned int period_us)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(period_us / 1000000U);
    delay.tv_nsec = (long)(period_us % 1000000U) * 1000L;

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stop_requested) {
    }
}

static void run_loop(const lsc_options_t *options, lsc_pins_t *pins)
{
    bool previous_trigger = false;
    uint64_t reconnect_after = 0U;
    int socket_fd = -1;

    while (!stop_requested) {
        bool trigger = *pins->tx_trigger != 0;
        uint64_t now = monotonic_milliseconds();

        if (*pins->enable == 0) {
            disconnect_socket(&socket_fd, pins);
            *pins->bus_off = 0;
            reconnect_after = 0U;
        } else {
            if (socket_fd < 0 && now >= reconnect_after) {
                socket_fd = open_can_socket(options->interface_name);
                if (socket_fd < 0) {
                    record_system_error(pins, errno);
                    reconnect_after = now + RECONNECT_DELAY_MS;
                } else {
                    *pins->connected = 1;
                    *pins->bus_off = 0;
                    *pins->last_error = 0;
                }
            }

            if (socket_fd >= 0 && receive_frames(socket_fd, pins) != 0) {
                int error_number = errno;
                record_system_error(pins, error_number);
                disconnect_socket(&socket_fd, pins);
                reconnect_after = now + RECONNECT_DELAY_MS;
            }

            if (trigger && !previous_trigger) {
                if (socket_fd < 0) {
                    record_system_error(pins, ENOTCONN);
                } else if (transmit_frame(socket_fd, pins) != 0) {
                    int error_number = errno;
                    record_system_error(pins, error_number);
                    if (error_requires_reconnect(error_number)) {
                        disconnect_socket(&socket_fd, pins);
                        reconnect_after = now + RECONNECT_DELAY_MS;
                    }
                }
            }
        }

        previous_trigger = trigger;
        sleep_for_period(options->period_us);
    }

    disconnect_socket(&socket_fd, pins);
}

int main(int argc, char **argv)
{
    lsc_options_t options;
    lsc_pins_t *pins;
    int component_id;
    int result;

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    component_id = hal_init(options.component_name);
    if (component_id < 0) {
        fprintf(stderr, "hal_init(%s) failed: %d\n", options.component_name, component_id);
        return EXIT_FAILURE;
    }

    pins = hal_malloc(sizeof(*pins));
    if (pins == NULL) {
        fprintf(stderr, "hal_malloc failed\n");
        hal_exit(component_id);
        return EXIT_FAILURE;
    }
    memset(pins, 0, sizeof(*pins));

    result = export_pins(component_id, options.component_name, pins);
    if (result < 0) {
        fprintf(stderr, "Failed to export HAL pins: %d\n", result);
        hal_exit(component_id);
        return EXIT_FAILURE;
    }

    initialize_pins(pins);
    result = hal_ready(component_id);
    if (result < 0) {
        fprintf(stderr, "hal_ready failed: %d\n", result);
        hal_exit(component_id);
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "%s: interface=%s period=%u us\n",
            options.component_name,
            options.interface_name,
            options.period_us);
    run_loop(&options, pins);
    hal_exit(component_id);
    return EXIT_SUCCESS;
}
