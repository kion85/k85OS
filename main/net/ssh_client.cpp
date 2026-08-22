#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "ssh_client.h"
#include "esp_heap_caps.h"
#include "ui/common.h"
#include "ui/text_input.h"
#include "core/input.h"
#include "M5Unified.h"

#include "wolfssh/ssh.h"

#include "lwip/sockets.h"
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Параметры соединения, вводимые пользователем, и объект синхронизации с
// вызывающей задачей (меню). Хендшейк/крипто-операции wolfSSH требуют
// заметно больше стека, чем есть у главной задачи (8KB) — поэтому вся
// сессия целиком выполняется в отдельной задаче с увеличенным стеком,
// а вызывающая сторона просто ждёт на семафоре.
struct K85SshClientParams {
    char host[64];
    char port_str[8];
    char user[32];
    char pass[64];
    SemaphoreHandle_t done_sem;
};

static void k85_ssh_client_wait_ab(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void k85_ssh_client_error(const char *msg) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s\nA+B=back", msg);
    k85_show_message(buf);
    k85_ssh_client_wait_ab();
}

// Отвечает на запрос авторизации библиотеки со стороны клиента — просто
// подставляет введённый пользователем пароль в структуру ответа.
static int ssh_client_auth_cb(byte authType, WS_UserAuthData *authData, void *ctx) {
    K85SshClientParams *p = (K85SshClientParams *)ctx;
    if (authType != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;

    authData->sf.password.password = (const byte *)p->pass;
    authData->sf.password.passwordSz = (word32)strlen(p->pass);
    return WOLFSSH_USERAUTH_SUCCESS;
}

// Показывает многострочный ответ сервера на экране (k85_area_show сама
// ждёт A+B перед возвратом).
static void k85_ssh_client_show_response(char *buf) {
    if (buf[0] == 0) {
        const char *empty[] = { "(no response)" };
        k85_area_show(empty, 1, "SSH");
        return;
    }
    const char *lines[16];
    int count = 0;
    char *p = buf;
    while (*p && count < 16) {
        lines[count++] = p;
        char *nl = strpbrk(p, "\r\n");
        if (!nl) break;
        while (*nl == '\r' || *nl == '\n') { *nl = 0; nl++; }
        p = nl;
    }
    if (count == 0) count = 1; // пустая строка на всякий случай, чтобы не звать show с 0
    k85_area_show(lines, count, "SSH");
}

// Интерактивный цикл: ввод команды -> отправка -> сбор ответа с коротким
// таймаутом простоя -> показ на экране -> следующая команда, либо выход.
static void k85_ssh_client_interactive(WOLFSSH *ssh, int sock) {
    // Короткий таймаут чтения именно для интерактивной фазы — чтобы цикл
    // сбора ответа не залипал надолго на пустых чтениях.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static char resp_buf[512];

    while (true) {
        char cmd[96] = "";
        if (!k85_text_input("SSH command:", "", cmd, sizeof(cmd))) break; // Exit = выход

        char send_buf[100];
        int n = snprintf(send_buf, sizeof(send_buf), "%s\r\n", cmd);
        int sret = wolfSSH_stream_send(ssh, (byte *)send_buf, n);
        if (sret <= 0) {
            k85_ssh_client_error("Connection lost");
            break;
        }

        size_t total = 0;
        resp_buf[0] = 0;
        TickType_t idle_start = xTaskGetTickCount();
        bool got_any = false;

        while (total < sizeof(resp_buf) - 1) {
            byte chunk[128];
            int r = wolfSSH_stream_read(ssh, chunk, sizeof(chunk));
            if (r > 0) {
                size_t copy = (size_t)r;
                if (total + copy >= sizeof(resp_buf) - 1) copy = sizeof(resp_buf) - 1 - total;
                memcpy(resp_buf + total, chunk, copy);
                total += copy;
                resp_buf[total] = 0;
                idle_start = xTaskGetTickCount();
                got_any = true;
            } else if (r == WS_WANT_READ) {
                uint32_t idle_ms = (xTaskGetTickCount() - idle_start) * portTICK_PERIOD_MS;
                // если уже что-то получили — короткая пауза простоя (800мс) значит,
                // что сервер закончил отвечать; если ещё ничего не было — ждём дольше (3с)
                uint32_t limit_ms = got_any ? 800 : 3000;
                if (idle_ms > limit_ms) break;
            } else {
                // WS_EOF или другая ошибка — соединение закрыто сервером
                break;
            }
        }

        k85_ssh_client_show_response(resp_buf);
    }
}

static void ssh_client_task(void *arg) {
    K85SshClientParams *p = (K85SshClientParams *)arg;

    int sock = -1;
    WOLFSSH_CTX *ctx = nullptr;
    WOLFSSH *ssh = nullptr;
    bool ok = true;
    bool wolfssh_inited = false;

    int port = atoi(p->port_str);
    if (port <= 0 || port > 65535) port = 22;

    if (ok) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) { k85_ssh_client_error("Socket create failed"); ok = false; }
    }

    struct sockaddr_in addr = {};
    if (ok) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, p->host, &addr.sin_addr) != 1) {
            k85_ssh_client_error("Invalid IP address");
            ok = false;
        }
    }

    if (ok) {
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        k85_show_message("Connecting...");
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            k85_ssh_client_error("TCP connect failed\n(host unreachable?)");
            ok = false;
        }
    }

    if (ok) {
        wolfSSH_Init();
        wolfssh_inited = true;
        ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, nullptr);
        if (!ctx) { k85_ssh_client_error("SSH context init failed"); ok = false; }
    }

    if (ok) {
        wolfSSH_SetUserAuth(ctx, ssh_client_auth_cb);
        ssh = wolfSSH_new(ctx);
        if (!ssh) { k85_ssh_client_error("SSH session init failed"); ok = false; }
    }

    if (ok) {
        wolfSSH_SetUserAuthCtx(ssh, p);
        wolfSSH_SetUsername(ssh, p->user);
        wolfSSH_set_fd(ssh, sock);

        k85_show_message("SSH handshake...");
        int cret = wolfSSH_connect(ssh);
        if (cret != WS_SUCCESS) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Handshake/auth failed\ncode=%d", cret);
            k85_ssh_client_error(buf);
            ok = false;
        }
    }

    if (ok) {
        int cret = wolfSSH_SetChannelType(ssh, WOLFSSH_SESSION_SHELL, nullptr, 0);
        if (cret != WS_SUCCESS) {
            k85_ssh_client_error("Shell channel request failed");
            ok = false;
        }
    }

    if (ok) {
        k85_ssh_client_interactive(ssh, sock);
    }

    if (ssh) {
        wolfSSH_stream_exit(ssh, 0);
        wolfSSH_free(ssh);
    }
    if (ctx) wolfSSH_CTX_free(ctx);
    if (wolfssh_inited) wolfSSH_Cleanup();
    if (sock >= 0) close(sock);

    xSemaphoreGive(p->done_sem);
    vTaskDelete(nullptr);
}

void k85_run_ssh_client(void) {
    static K85SshClientParams params;
    memset(&params, 0, sizeof(params));

    if (!k85_text_input("SSH host (IP):", "", params.host, sizeof(params.host))) return;
    if (params.host[0] == 0) return;

    if (!k85_text_input("Port:", "22", params.port_str, sizeof(params.port_str))) return;

    if (!k85_text_input("Username:", "", params.user, sizeof(params.user))) return;

    // Примечание: text_input не поддерживает маскировку — пароль будет
    // виден на экране во время ввода.
    if (!k85_text_input("Password (visible):", "", params.pass, sizeof(params.pass))) return;

    params.done_sem = xSemaphoreCreateBinary();
    if (!params.done_sem) {
        k85_show_message("Out of memory\nA+B=back");
        k85_ssh_client_wait_ab();
        return;
    }

    // Хендшейк wolfSSH требует заметно больше стека, чем есть у вызывающей
    // (главной) задачи — поэтому вся сессия целиком уходит в отдельную
    // задачу с увеличенным стеком, аналогично тому, что мы делали для
    // SSH-сервера.
    TaskHandle_t task = nullptr;
    BaseType_t created = xTaskCreate(ssh_client_task, "k85_sshc", 16384, &params, 5, &task);
    if (created != pdPASS) {
        vSemaphoreDelete(params.done_sem);
        k85_show_message("Task create failed\n(out of RAM?)\nA+B=back");
        k85_ssh_client_wait_ab();
        return;
    }

    xSemaphoreTake(params.done_sem, portMAX_DELAY);
    vSemaphoreDelete(params.done_sem);

    // Затираем пароль из памяти сразу после использования.
    memset(params.pass, 0, sizeof(params.pass));
}

#pragma GCC diagnostic pop