#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>

#define TYPE_LEN 12

struct sensor_message
{
    char type[TYPE_LEN];
    int coords[2];
    float measurement;
};

static char g_type[TYPE_LEN];
static int g_x, g_y;
static float g_measurement;   
static float g_min_measurement;
static float g_max_measurement;
static int g_interval;

typedef struct
{
    int x, y;
    float measurement;
} known_sensor;

static known_sensor g_known[128];
static int g_known_count = 0;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_sockfd;

void print_usage()
{
    fprintf(stderr,
            "Usage: ./client <server_ip> <port> -type <temperature|humidity|air_quality> -coords <x> <y>\n");
}

static float dist(int x1, int y1, int x2, int y2)
{
    return sqrtf((float)((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2)));
}

static void update_known_sensor(int rx, int ry, float meas)
{
    if (meas < 0.0f)
    {
        for (int i = 0; i < g_known_count; i++)
        {
            if (g_known[i].x == rx && g_known[i].y == ry)
            {
                g_known[i] = g_known[g_known_count - 1];
                g_known_count--;
                return;
            }
        }
        return;
    }

    for (int i = 0; i < g_known_count; i++)
    {
        if (g_known[i].x == rx && g_known[i].y == ry)
        {
            g_known[i].measurement = meas;
            return;
        }
    }
    if (g_known_count < 128) 
    {
        g_known[g_known_count].x = rx;
        g_known[g_known_count].y = ry;
        g_known[g_known_count].measurement = meas;
        g_known_count++;
    }
}

static const char *apply_correction_if_neighbor(int rx, int ry, float remote)
{
    if (rx == g_x && ry == g_y)
    {
        return "same location";
    }

    if (remote < 0.0f)
    {
        return "removed";
    }

    float d_remote = dist(g_x, g_y, rx, ry);

    // Se ainda não houver três sensores conhecidos, automaticamente aplicamos a correção
    if (g_known_count <= 3)
    {
        float delta = (remote - g_measurement) * (0.1f / (d_remote + 1.0f));
        g_measurement = g_measurement + delta;

        // Garante que g_measurement permaneça dentro dos limites definidos
        if (g_measurement < g_min_measurement)
            g_measurement = g_min_measurement;
        if (g_measurement > g_max_measurement)
            g_measurement = g_max_measurement;

        static char msg[64];
        snprintf(msg, sizeof(msg), "correction of %.4f", delta);
        return msg;
    }
    else
    {
        // Determinar quantos sensores conhecidos estão mais próximos do que o sensor remoto
        int count_closer = 0;
        for (int i = 0; i < g_known_count; i++)
        {
            float d_known = dist(g_x, g_y, g_known[i].x, g_known[i].y);
            if (d_known < d_remote)
                count_closer++;
        }
        if (count_closer <= 3)
        {
            float delta = (remote - g_measurement) * (0.1f / (d_remote + 1.0f));
            g_measurement = g_measurement + delta;

            if (g_measurement < g_min_measurement)
                g_measurement = g_min_measurement;
            if (g_measurement > g_max_measurement)
                g_measurement = g_max_measurement;

            static char msg[64];
            snprintf(msg, sizeof(msg), "correction of %.4f", delta);
            return msg;
        }
        else
        {
            return "not neighbor";
        }
    }
}

void *receiver_thread(void *arg)
{
    (void)arg; // unused
    struct sensor_message rmsg;

    while (1)
    {
        ssize_t n = recv(g_sockfd, &rmsg, sizeof(rmsg), 0);
        if (n <= 0)
        {
            fprintf(stderr, "Servidor desconectado.\n");
            exit(1);
        }

        pthread_mutex_lock(&g_lock);

        update_known_sensor(rmsg.coords[0], rmsg.coords[1], rmsg.measurement);
        const char *action = apply_correction_if_neighbor(rmsg.coords[0],
                                                          rmsg.coords[1],
                                                          rmsg.measurement);

        printf("log:\n%s sensor in (%d,%d)\nmeasurement: %.4f\naction: %s\n\n",
               rmsg.type, rmsg.coords[0], rmsg.coords[1], rmsg.measurement, action);
        fflush(stdout);

        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

float rand_between(float minf, float maxf)
{
    float scale = rand() / (float)RAND_MAX; // [0, 1]
    return minf + scale * (maxf - minf);
}

int main(int argc, char *argv[])
{
    srand(time(NULL));

    if (argc < 7)
    {
        fprintf(stderr, "Error: Invalid number of arguments\n");
        print_usage();
        return 1;
    }

    char *server_ip = argv[1];
    char *server_port = argv[2];

    int idx_type = -1;
    int idx_coords = -1;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "-type") == 0)
        {
            idx_type = i;
        }
        if (strcmp(argv[i], "-coords") == 0)
        {
            idx_coords = i;
        }
    }

    // Verifica prioridade dos erros
    if (idx_type < 0)
    {
        fprintf(stderr, "Error: Expected '-type' argument\n");
        print_usage();
        return 1;
    }
    if (idx_type + 1 >= argc)
    {
        fprintf(stderr, "Error: Invalid number of arguments\n");
        print_usage();
        return 1;
    }
    strncpy(g_type, argv[idx_type + 1], TYPE_LEN - 1);
    g_type[TYPE_LEN - 1] = '\0'; // Garantir terminação

    if (strcmp(g_type, "temperature") != 0 &&
        strcmp(g_type, "humidity") != 0 &&
        strcmp(g_type, "air_quality") != 0)
    {
        fprintf(stderr, "Error: Invalid sensor type\n");
        print_usage();
        return 1;
    }

    if (idx_coords < 0)
    {
        fprintf(stderr, "Error: Expected '-coords' argument\n");
        print_usage();
        return 1;
    }

    if (idx_coords + 2 >= argc)
    {
        // faltou algo nos argumentos
        fprintf(stderr, "Error: Invalid number of arguments\n");
        print_usage();
        return 1;
    }

    g_x = atoi(argv[idx_coords + 1]);
    g_y = atoi(argv[idx_coords + 2]);

    // Verifica range 0-9 para coordenadas
    if (g_x < 0 || g_x > 9 || g_y < 0 || g_y > 9)
    {
        fprintf(stderr, "Error: Coordinates must be in the range 0-9\n");
        print_usage();
        return 1;
    }

    if (strcmp(g_type, "temperature") == 0)
    {
        g_min_measurement = 20.0f;
        g_max_measurement = 40.0f;
        g_interval = 5; // 5 segundos
    }
    else if (strcmp(g_type, "humidity") == 0)
    {
        g_min_measurement = 10.0f;
        g_max_measurement = 90.0f;
        g_interval = 7; // 7 segundos
    }
    else if (strcmp(g_type, "air_quality") == 0)
    {
        g_min_measurement = 15.0f;
        g_max_measurement = 30.0f;
        g_interval = 10; // 10 segundos
    }
    g_measurement = rand_between(g_min_measurement, g_max_measurement);

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // tanto faz v4 ou v6, o IP do user define
    hints.ai_socktype = SOCK_STREAM;

    int rv = getaddrinfo(server_ip, server_port, &hints, &res);
    if (rv != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    int connected = 0;
    for (p = res; p != NULL; p = p->ai_next)
    {
        g_sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (g_sockfd == -1)
            continue;
        if (connect(g_sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(g_sockfd);
            continue;
        }
        connected = 1;
        break;
    }
    freeaddrinfo(res);

    if (!connected)
    {
        fprintf(stderr, "Error: could not connect to server\n");
        return 1;
    }

    struct sensor_message init_msg;
    memset(&init_msg, 0, sizeof(init_msg));
    strncpy(init_msg.type, g_type, TYPE_LEN - 1);
    init_msg.type[TYPE_LEN - 1] = '\0'; // Garantir terminação
    init_msg.coords[0] = g_x;
    init_msg.coords[1] = g_y;
    init_msg.measurement = g_measurement;
    send(g_sockfd, &init_msg, sizeof(init_msg), 0);

    pthread_t tid;
    pthread_create(&tid, NULL, receiver_thread, NULL);
    pthread_detach(tid);

    while (1)
    {
        sleep(g_interval);

        // Monta mensagem
        pthread_mutex_lock(&g_lock);
        struct sensor_message msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.type, g_type, TYPE_LEN - 1);
        msg.type[TYPE_LEN - 1] = '\0'; 
        msg.coords[0] = g_x;
        msg.coords[1] = g_y;
        msg.measurement = g_measurement;
        pthread_mutex_unlock(&g_lock);

        send(g_sockfd, &msg, sizeof(msg), 0);
    }

    close(g_sockfd);
    return 0;
}
