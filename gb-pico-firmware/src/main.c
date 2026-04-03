#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/malloc.h"

#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "retroachievements.h"
#include "gb_cpu_emu.h"

#include "hardware/timer.h"
#include <limits.h>
#include <math.h>

#define UART_ID uart0
#define BAUD_RATE 115200

#define UART_TX_PIN 28 // GPIO pin for TX
#define UART_RX_PIN 29 // GPIO pin for RX

// CRC32 global variables
uint32_t crcBegin = 0xFFFFFFFF;
uint32_t crcEnd = 0xFFFFFFFF;

/*
 * states
 */

uint8_t state = 0;
int nes_reseted = 0;
char md5[33];
uint8_t request_ongoing = 0;
uint32_t last_request = 0;
char ra_token[32];
char ra_user[256];
/*
 * Serial buffers
 */

#define SERIAL_BUFFER_SIZE 32768
u_char serial_buffer[SERIAL_BUFFER_SIZE];
u_char *serial_buffer_head = serial_buffer;
// u_char command[256];

bool prefix(const char *pre, const char *str)
{
    return strncmp(pre, str, strlen(pre)) == 0;
}

// RCHEEVOS

typedef struct
{
    rc_client_server_callback_t callback;
    void *callback_data;
} async_callback_data;

rc_client_t *g_client = NULL;
static void *g_callback_userdata = &g_client; /* dummy data */
char rcheevos_userdata[16];

async_callback_data async_data;

typedef struct
{
    uint8_t id;
    async_callback_data async_data;
} async_callback_data_id;

#define MAX_ASYNC_CALLBACKS 5
async_callback_data_id async_handlers[MAX_ASYNC_CALLBACKS];
uint8_t async_handlers_index = 0;

uint8_t request_id = 0;

/*
 * FIFO for achievements the user won
 */

#define FIFO_SIZE 5

typedef struct
{
    uint32_t buffer[FIFO_SIZE];
    int head;
    int tail;
    int count;
} FIFO_t;

void fifo_init(FIFO_t *fifo)
{
    fifo->head = 0;
    fifo->tail = 0;
    fifo->count = 0;
}

bool fifo_is_empty(FIFO_t *fifo)
{
    return fifo->count == 0;
}

bool fifo_is_full(FIFO_t *fifo)
{
    return fifo->count == FIFO_SIZE;
}

bool fifo_enqueue(FIFO_t *fifo, uint32_t value)
{
    if (fifo_is_full(fifo))
    {
        return false; // FIFO cheia
    }
    fifo->buffer[fifo->tail] = value;
    fifo->tail = (fifo->tail + 1) % FIFO_SIZE;
    fifo->count++;
    return true;
}

bool fifo_dequeue(FIFO_t *fifo, uint32_t *value)
{
    if (fifo_is_empty(fifo))
    {
        return false; // FIFO vazia
    }
    *value = fifo->buffer[fifo->head];
    fifo->head = (fifo->head + 1) % FIFO_SIZE;
    fifo->count--;
    return true;
}

void fifo_print(FIFO_t *fifo)
{
    printf("FIFO: ");
    int index = fifo->head;
    for (int i = 0; i < fifo->count; i++)
    {
        printf("%u ", fifo->buffer[index]);
        index = (index + 1) % FIFO_SIZE;
    }
    printf("\n");
}

FIFO_t achievements_fifo;

// Read memory directly from emulator's memory[] array
static uint32_t read_memory_do_nothing(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
    return num_bytes;
}

static uint32_t read_memory_ingame(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client)
{
    for (uint32_t j = 0; j < num_bytes; j++)
    {
        buffer[j] = memory[(address + j) & 0xFFFF];
    }
    return num_bytes;
}

static void rc_client_login_callback(int result, const char *error_message, rc_client_t *client, void *callback_userdata)
{
    if (result == RC_OK)
    {
        printf("Login success\n");
        state = 6; // load game
    }
    else
    {
        printf("Login failed\n");
    }
}

static void rc_client_load_game_callback(int result, const char *error_message, rc_client_t *client, void *callback_userdata)
{
    if (result == RC_OK)
    {
        state = 8; // emulator running, process frames
        if (rc_client_is_game_loaded(g_client))
        {
            printf("Game loaded\n");
            const rc_client_game_t *game = rc_client_get_game_info(g_client);
            char url[256];
            rc_client_game_get_image_url(game, url, sizeof(url));
            char aux[512];
            sprintf(aux, "GAME_INFO=%lu;%s;%s\r\n", (unsigned long)game->id, game->title, url);
            printf(aux);
            uart_puts(UART_ID, aux);
        }
        rc_client_set_read_memory_function(g_client, read_memory_ingame);
        rc_client_do_frame(g_client);
        // Emulator already running on Core 1 since boot
    }
    else
    {
        printf("Game not loaded\n");
    }
}

static void achievement_triggered(const rc_client_achievement_t *achievement)
{
    fifo_enqueue(&achievements_fifo, achievement->id);
}

static void event_handler(const rc_client_event_t *event, rc_client_t *client)
{
    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        achievement_triggered(event->achievement);
        break;

    default:
        printf("Unhandled event %d\n", event->type);
        break;
    }
}

// This is the callback function for the asynchronous HTTP call (which is not provided in this example)
static void http_callback(int status_code, const char *content, size_t content_size, void *userdata, const char *error_message)
{
    // Prepare a data object to pass the HTTP response to the callback
    rc_api_server_response_t server_response;
    memset(&server_response, 0, sizeof(server_response));
    server_response.body = content;
    server_response.body_length = content_size;
    server_response.http_status_code = status_code;

    // handle non-http errors (socket timeout, no internet available, etc)
    if (status_code == 0 && error_message)
    {
        // assume no server content and pass the error through instead
        server_response.body = error_message;
        server_response.body_length = strlen(error_message);
        // Let rc_client know the error was not catastrophic and could be retried. It may decide to retry or just
        // immediately pass the error to the callback. To prevent possible retries, use RC_API_SERVER_RESPONSE_CLIENT_ERROR.
        server_response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }

    // Get the rc_client callback and call it
    async_callback_data *async_data = (async_callback_data *)userdata;
    async_data->callback(&server_response, async_data->callback_data);
}

rc_clock_t get_pico_millisecs(const rc_client_t *client)
{
    return to_ms_since_boot(get_absolute_time());
}

// This is the HTTP request dispatcher that is provided to the rc_client. Whenever the client
// needs to talk to the server, it will call this function.
static void server_call(const rc_api_request_t *request,
                        rc_client_server_callback_t callback, void *callback_data, rc_client_t *client)
{
    char buffer[512];
    async_data.callback = callback;
    async_data.callback_data = callback_data;
    char method[8];
    if (request->post_data)
    {
        strcpy(method, "POST");
    }
    else
    {
        strcpy(method, "GET");
    }
    sprintf(buffer, "REQ=%02hhX;M:%s;U:%s;D:%s\r\n", request_id, method, request->url, request->post_data);
    async_handlers[async_handlers_index].id = request_id;
    async_handlers[async_handlers_index].async_data.callback = callback;
    async_handlers[async_handlers_index].async_data.callback_data = callback_data;
    async_handlers_index = async_handlers_index + 1 % MAX_ASYNC_CALLBACKS;
    request_id += 1;
    printf("REQ=%s\n", request->post_data); // DEBUG
    request_ongoing += 1;
    last_request = to_ms_since_boot(get_absolute_time());
    uart_puts(UART_ID, buffer);
}

#include <malloc.h>

const char pico_version_command[] = "PICO_FIRMWARE_VERSION=0.7\r\n";
const char nes_reseted_command[] = "NES_RESETED\r\n";
const char buffer_overflow_command[] = "BUFFER_OVERFLOW\r\n";

uint64_t last_frame_processed = 0;

int main()
{
    // setup
    stdio_init_all();
    set_sys_clock_khz(250000, true);

    // Start CPU emulator on Core 1 immediately so it tracks from boot ROM
    // setupPIO() inside emu_core1_entry will handle GPIO init for bus pins
    multicore_launch_core1(emu_core1_entry);

    uart_init(UART_ID, BAUD_RATE);

    // Configura os pinos GPIO para a UART
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Habilita a porta UART
    uart_set_hw_flow(UART_ID, true, true);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);
    printf(pico_version_command);
    memset(serial_buffer, '\0', SERIAL_BUFFER_SIZE);

    // Track emulator debug state changes from Core 1
    static int last_emu_state = -1;
    static uint32_t last_stall_print = 0;
    static const char *last_logged_error = NULL;

    // Core 0 main loop
    while (true)
    {
        /* --- Print emulator debug milestones (Core 1 never printf's) --- */
        int cur_emu_state = emu_debug_state;
        if (cur_emu_state != last_emu_state) {
            switch (cur_emu_state) {
                case EMU_STATE_STARTED:
                    printf("EMU: Core 1 started\n"); break;
                case EMU_STATE_WAITING_0100:
                    printf("EMU: Waiting for 0x0100...\n"); break;
                case EMU_STATE_RUNNING:
                    printf("EMU: 0x0100 reached, cycleRatio=%u, first_raw=0x%08X, fifo=%u\n",
                           (unsigned)emu_debug_cycle_ratio, (unsigned)emu_debug_first_raw,
                           (unsigned)emu_debug_fifo_level);
                    break;
                case EMU_STATE_ERROR:
                    printf("EMU error (state): %s (opcode 0x%02X)\n",
                           emu_debug_error ? emu_debug_error : "unknown",
                           (unsigned)errorOpcode);
                    break;
            }
            last_emu_state = cur_emu_state;
        }

        /* --- Catch errors independently (survives state race condition) --- */
        {
            const char *cur_err = (const char *)emu_debug_error;
            if (cur_err != NULL && cur_err != last_logged_error) {
                printf("EMU error: %s (opcode 0x%02X, addr=0x%04X, SP=0x%04X, opcodes=%u, stalls=%u)\n",
                       cur_err, (unsigned)errorOpcode,
                       (unsigned)emu_debug_last_addr,
                       (unsigned)emu_debug_sp,
                       (unsigned)emu_debug_opcode_count,
                       (unsigned)emu_debug_stall_count);
                /* Print bus history (last N entries from ring buffer) */
                uint32_t tc = emu_trace_count;
                if (tc > EMU_TRACE_SIZE) tc = EMU_TRACE_SIZE;
                printf("EMU history (%u recent bus reads):\n", (unsigned)tc);
                for (uint32_t i = 0; i < tc; i++) {
                    uint32_t rr = emu_trace_raw[i];
                    printf("  [%u] raw=0x%08X  addr=0x%04X data=0x%02X ctrl=0x%02X\n",
                           (unsigned)i,
                           (unsigned)rr,
                           (unsigned)(rr & 0xFFFF),
                           (unsigned)((rr >> 24) & 0xFF),
                           (unsigned)((rr >> 16) & 0xFF));
                }
                /* Print first opcode executions */
                uint32_t nops = emu_debug_opcode_count;
                if (nops > EMU_OP_TRACE_SIZE) nops = EMU_OP_TRACE_SIZE;
                printf("EMU first %u ops:\n", (unsigned)nops);
                for (uint32_t i = 0; i < nops; i++) {
                    uint32_t op = emu_trace_ops[i];
                    printf("  op[%u] addr=0x%04X opcode=0x%02X\n",
                           (unsigned)i, (unsigned)(op >> 16), (unsigned)(op & 0xFF));
                }
                /* Print early bus trace (first N reads from 0x0100 onward) */
                uint32_t ebc = emu_early_bus_count;
                if (ebc > EMU_EARLY_BUS_SIZE) ebc = EMU_EARLY_BUS_SIZE;
                printf("EMU early bus (%u entries, [0]=0x0100, [1..5]=readahead):\n", (unsigned)ebc);
                for (uint32_t i = 0; i < ebc; i++) {
                    uint32_t eb = emu_early_bus[i];
                    printf("  eb[%u] raw=0x%08X addr=0x%04X data=0x%02X ctrl=0x%02X\n",
                           (unsigned)i,
                           (unsigned)eb,
                           (unsigned)(eb & 0xFFFF),
                           (unsigned)((eb >> 24) & 0xFF),
                           (unsigned)((eb >> 16) & 0xFF));
                }
                last_logged_error = cur_err;
            } else if (cur_err == NULL) {
                last_logged_error = NULL;
            }
        }

        /* Periodic stall/opcode count report (every ~2 seconds) */
        if (cur_emu_state == EMU_STATE_RUNNING) {
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms - last_stall_print > 2000) {
                last_stall_print = now_ms;
                printf("EMU: opcodes=%u vblanks=%u sp_resyncs=%u stalls=%u\n",
                       (unsigned)emu_debug_opcode_count,
                       (unsigned)emu_debug_vblank_count,
                       (unsigned)emu_debug_sp_resync_count,
                       (unsigned)emu_debug_stall_count);
            }
        }

        // handle on going request and timeout
        if (request_ongoing > 0)
        {
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            if (current_time - last_request > 30000)
            {
                printf("request timeout\n");
                request_ongoing = 0;
            }
        }

        // if there is no request on the fly and there is an achievement to be sent
        if (request_ongoing == 0 && fifo_is_empty(&achievements_fifo) == false)
        {
            uint32_t achievement_id;
            fifo_dequeue(&achievements_fifo, &achievement_id);
            const rc_client_achievement_t *achievement = rc_client_get_achievement_info(g_client, achievement_id);
            char url[128];
            const char *title = achievement->title;
            rc_client_achievement_get_image_url(achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url));
            char aux[512];
            memset(aux, 0, 512);
            sprintf(aux, "A=%lu;%s;%s\r\n", (unsigned long)achievement_id, title, url);
            uart_puts(UART_ID, aux);
            printf(aux);
        }

        if (state == 1)
        {
            // read CRC
            // handleCRC32();
            state = 2;
        }
        if (state == 6)
        {
            // load the game
            rc_client_begin_load_game(g_client, md5, rc_client_load_game_callback, g_callback_userdata);
            state = 7;
        }
        if (state == 8)
        {
            // CPU emulator is running on Core 1
            // Process a frame when the emulator signals VBlank
            if (emu_new_frame)
            {
                emu_new_frame = false;

                uint32_t vb = emu_debug_vblank_count;
                if (vb <= 5 || (vb % 600) == 0) {
                    printf("EMU: VBlank #%u\n", (unsigned)vb);
                }

                if (nes_reseted == 0)
                {
                    nes_reseted = 1;
                    uart_puts(UART_ID, nes_reseted_command);
                }

                rc_client_do_frame(g_client);
                last_frame_processed = to_ms_since_boot(get_absolute_time());
            }
            else
            {
                // Fallback: ensure we process at least every ~17ms even if VBlank signal is missed
                uint64_t now = to_ms_since_boot(get_absolute_time());
                if ((now - last_frame_processed) > 17)
                {
                    rc_client_do_frame(g_client);
                    last_frame_processed = now;
                }
            }
        }
        if (uart_is_readable(UART_ID))
        {

            char received_char = uart_getc(UART_ID);
            // printf(serial_buffer);
            serial_buffer_head[0] = received_char;
            serial_buffer_head += 1;
            if (serial_buffer_head - serial_buffer == SERIAL_BUFFER_SIZE)
            {
                memset(serial_buffer, 0, SERIAL_BUFFER_SIZE);
                printf(buffer_overflow_command); // BUFFER_OVERFLOW\r\n
                continue;
            }
            char *pos = NULL;
            char *current_char = serial_buffer_head - 1;
            if (current_char[0] == '\n')
            {
                char *previous_char = current_char - 1;
                if (previous_char[0] == '\r')
                {
                    pos = previous_char;
                }
            }
            if (pos != NULL)
            {
                if (((unsigned char *)pos) - serial_buffer == 0)
                {
                    memset(serial_buffer, 0, 2); // Clear the buffer since we are reading char by char
                    serial_buffer_head = serial_buffer;
                    continue;
                }
                int len = serial_buffer_head - serial_buffer;
                serial_buffer_head[0] = '\0';
                char *command;
                command = serial_buffer;

                // printf("CMD=%s\r\n", command);
                if (prefix("RESP=", command))
                {
                    printf("L:RESP\n");
                    request_ongoing -= 1;
                    char *response_ptr = command + 5;
                    char aux[8];
                    strncpy(aux, response_ptr, 2);
                    aux[2] = '\0';
                    uint8_t request_id = (uint8_t)strtol(aux, NULL, 16);
                    response_ptr += 3;
                    strncpy(aux, response_ptr, 3);
                    aux[3] = '\0';
                    response_ptr += 4;
                    uint16_t http_code = (uint16_t)strtol(aux, NULL, 16);
                    for (int i = 0; i < MAX_ASYNC_CALLBACKS; i += 1)
                    {
                        if (async_handlers[i].id == request_id)
                        {
                            if (request_id == 2)
                            {
                                printf("RESP=%s\n", response_ptr);
                            }
                            async_callback_data async_data = async_handlers[i].async_data;
                            http_callback(http_code, response_ptr, strlen(response_ptr), &async_data, NULL);
                            break;
                        }
                    }
                }
                else if (prefix("TOKEN_AND_USER", command))
                {
                    // example TOKEN_AND_USER=odelot,token
                    printf("L:TOKEN_AND_USER\n");
                    char *token_ptr = command + 15;
                    int comma_index = 0;
                    len = strlen(token_ptr);
                    for (int i = 0; i < len; i += 1)
                    {
                        if (token_ptr[i] == ',')
                        {
                            comma_index = i;
                            break;
                        }
                    }
                    memset(ra_token, '\0', 32);
                    memset(ra_user, '\0', 256);
                    strncpy(ra_token, token_ptr, comma_index);
                    strncpy(ra_user, token_ptr + comma_index + 1, len - comma_index - 1 - 2);
                    printf("USER=%s\r\n", ra_user);
                    printf("TOKEN=%s\r\n", ra_token);
                }
                else if (prefix("CRC_FOUND_MD5", command))
                {
                    printf("L:CRC_FOUND_MD5\n");
                    char *md5_ptr = command + 14;
                    strncpy(md5, md5_ptr, 32);
                    md5[32] = '\0';
                    printf("MD5=%s\r\n", md5);
                }
                else if (prefix("RESET", command)) // RESET
                {
                    printf("L:RESET\r\n");
                    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
                    printf("pll_sys  = %dkHz\n", f_pll_sys);
                    fifo_init(&achievements_fifo);
                    state = 0;
                    nes_reseted = 0;
                    memset(md5, '\0', 33);
                    crcBegin = 0xFFFFFFFF;
                    // Note: Core 1 (emulator) will be stopped on next multicore_launch_core1
                    memset((void *)memory, 0, 0x10000);
                }
                else if (prefix("READ_CRC", command))
                {
                    printf("L:READ_CRC\n");
                    state = 1;
                    printf("STATE=%d\r\n", state);
                }
                else if (prefix("START_WATCH", command))
                {
                    printf("L:START_WATCH\n");
                    // init rcheevos

                    g_client = initialize_retroachievements_client(g_client, read_memory_do_nothing, server_call);
                    rc_client_get_user_agent_clause(g_client, rcheevos_userdata, sizeof(rcheevos_userdata)); // TODO: send to esp32 before doing requests
                    rc_client_set_event_handler(g_client, event_handler);
                    rc_client_set_get_time_millisecs_function(g_client, get_pico_millisecs);
                    rc_client_begin_login_with_token(g_client, ra_user, ra_token, rc_client_login_callback, g_callback_userdata);
                    state = 5;
                }
                memset(serial_buffer, 0, len); // Clear the buffer since we are reading char by char
                serial_buffer_head = serial_buffer;
            }
        }
    }
}