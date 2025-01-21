#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <stdbool.h>

#define TYPE_LEN 12
#define MAX_SENSORS 64 // Limite arbitrário de sensores por tipo

struct sensor_message
{
    char type[TYPE_LEN];
    int coords[2];    
    float measurement;
};

typedef struct
{
    int sockfd;
    char type[TYPE_LEN];
    int x, y;
} sensor_info;


// Lista global de sensores por tipo
static sensor_info g_temperature_list[MAX_SENSORS];
static int g_temperature_count = 0;

static sensor_info g_humidity_list[MAX_SENSORS];
static int g_humidity_count = 0;

static sensor_info g_airq_list[MAX_SENSORS];
static int g_airq_count = 0;

/* Mutex global para proteger acesso às listas */
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

void remove_sensor(int sockfd, const char *type);

void broadcast_message(struct sensor_message *msg, int except_sockfd)
{
    pthread_mutex_lock(&g_mutex);

    sensor_info *list = NULL;
    int *count_ptr = NULL;

    if (strcmp(msg->type, "temperature") == 0)
    {
        list = g_temperature_list;
        count_ptr = &g_temperature_count;
    }
    else if (strcmp(msg->type, "humidity") == 0)
    {
        list = g_humidity_list;
        count_ptr = &g_humidity_count;
    }
    else if (strcmp(msg->type, "air_quality") == 0)
    {
        list = g_airq_list;
        count_ptr = &g_airq_count;
    }
    else
    {
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    for (int i = *count_ptr - 1; i >= 0; i--)
    {
        ssize_t n = send(list[i].sockfd, msg, sizeof(*msg), 0);
        if (n < 0)
        {
            remove_sensor(list[i].sockfd, msg->type);
            close(list[i].sockfd);
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

/* Remove sensor de sua lista ao desconectar */
void remove_sensor(int sockfd, const char *type)
{
    sensor_info *list = NULL;
    int *count_ptr = NULL;

    if (strcmp(type, "temperature") == 0)
    {
        list = g_temperature_list;
        count_ptr = &g_temperature_count;
    }
    else if (strcmp(type, "humidity") == 0)
    {
        list = g_humidity_list;
        count_ptr = &g_humidity_count;
    }
    else if (strcmp(type, "air_quality") == 0)
    {
        list = g_airq_list;
        count_ptr = &g_airq_count;
    }
    else
    {
        return;
    }

    for (int i = 0; i < *count_ptr; i++)
    {
        if (list[i].sockfd == sockfd)
        {
            // Shift no array para remover o sensor
            list[i] = list[*count_ptr - 1];
            (*count_ptr)--;
            break;
        }
    }
}

/* Thread que trata cada cliente conectado */
void *client_thread(void *arg)
{
    sensor_info sinfo = *((sensor_info *)arg);
    free(arg);

    int sockfd = sinfo.sockfd;
    char sensor_type[TYPE_LEN];
    strcpy(sensor_type, sinfo.type);

    struct sensor_message msg;
    while (1)
    {
        ssize_t n = recv(sockfd, &msg, sizeof(msg), 0);
        if (n <= 0)
        {
            // Desconectou ou erro
            break;
        }

        printf("log:\n%s sensor in (%d,%d)\nmeasurement: %.4f\n\n",
               msg.type, msg.coords[0], msg.coords[1], msg.measurement);
        fflush(stdout);

        broadcast_message(&msg, sockfd);
    }

    struct sensor_message leave_msg;
    memset(&leave_msg, 0, sizeof(leave_msg));
    strncpy(leave_msg.type, sensor_type, TYPE_LEN);
    leave_msg.coords[0] = sinfo.x;
    leave_msg.coords[1] = sinfo.y;
    leave_msg.measurement = -1.0000f;
    

    printf("log:\n%s sensor in (%d,%d)\nmeasurement: -1.0000\n\n",
           leave_msg.type, leave_msg.coords[0], leave_msg.coords[1]);
    fflush(stdout);

    broadcast_message(&leave_msg, -1);

    pthread_mutex_lock(&g_mutex);
    remove_sensor(sockfd, sensor_type);
    pthread_mutex_unlock(&g_mutex);

    close(sockfd);
    return NULL;
}

int setup_server_socket(const char *address_family, const char *port)
{
    int sockfd;
    int rv;
    struct addrinfo hints, *servinfo, *p;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = (strcmp(address_family, "v6") == 0) ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // Para bind

    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rv));
        exit(EXIT_FAILURE);
    }

    for (p = servinfo; p != NULL; p = p->ai_next)
    {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1)
        {
            continue;
        }

        int yes = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            continue;
        }
        break;
    }

    if (p == NULL)
    {
        fprintf(stderr, "Failed to bind.\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, 10) == -1)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <v4|v6> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *address_family = argv[1];
    const char *port = argv[2];

    int server_sock = setup_server_socket(address_family, port);

    printf("Servidor iniciado (%s) na porta %s.\n", address_family, port);

    while (1)
    {
        struct sockaddr_storage their_addr;
        socklen_t sin_size = sizeof(their_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&their_addr, &sin_size);
        if (client_sock == -1)
        {
            perror("accept");
            continue;
        }

        // Primeira mensagem do cliente deve ser o "registro" do sensor
        struct sensor_message init_msg;
        ssize_t n = recv(client_sock, &init_msg, sizeof(init_msg), 0);
        if (n <= 0)
        {
            close(client_sock);
            continue;
        }


        sensor_info *new_sensor = (sensor_info *)malloc(sizeof(sensor_info));
        new_sensor->sockfd = client_sock;
        strncpy(new_sensor->type, init_msg.type, TYPE_LEN);
        new_sensor->x = init_msg.coords[0];
        new_sensor->y = init_msg.coords[1];

        pthread_mutex_lock(&g_mutex);
        if (strcmp(init_msg.type, "temperature") == 0)
        {
            if (g_temperature_count < MAX_SENSORS)
                g_temperature_list[g_temperature_count++] = *new_sensor;
            else
                fprintf(stderr, "Error: List of temperature is full.\n");
        }
        else if (strcmp(init_msg.type, "humidity") == 0)
        {
            if (g_humidity_count < MAX_SENSORS)
                g_humidity_list[g_humidity_count++] = *new_sensor;
            else
                fprintf(stderr, "Error:List of humidity is full.\n");
        }
        else if (strcmp(init_msg.type, "air_quality") == 0)
        {
            if (g_airq_count < MAX_SENSORS)
                g_airq_list[g_airq_count++] = *new_sensor;
            else
                fprintf(stderr, "Erro: List of air quality is full.\n");
        }
        else
        {
            free(new_sensor);
            pthread_mutex_unlock(&g_mutex);
            close(client_sock);
            continue;
        }
        pthread_mutex_unlock(&g_mutex);


        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, new_sensor);
        pthread_detach(tid);
    }

    close(server_sock);
    return 0;
}
