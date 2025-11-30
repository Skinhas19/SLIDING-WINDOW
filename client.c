#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define SERVER_PORT 9999
#define WINDOW_SIZE 5
#define TIMEOUT_SEC 1
#define MAX_PAYLOAD 1016 

// --- Estruturas (IDÊNTICAS AO SERVIDOR) ---

#define OP_UPLOAD 1
#define OP_DOWNLOAD 2

typedef struct {
    int operation;
    char filename[256];
} RequestPacket;

typedef struct {
    int seq_num;
    int data_len;
    char data[MAX_PAYLOAD];
} DataPacket;

typedef struct {
    int ack_num; 
} AckPacket;

// Slot da Janela de Transmissão (Emissor)
typedef struct {
    DataPacket packet;
    int is_acked;       
    int is_used;        
    time_t time_sent;   
} SenderSlot;

// Slot do Buffer de Recepção (Receptor)
typedef struct {
    DataPacket packet;
    int received;       
} ReceiverSlot;

void die(char *s) {
    perror(s);
    exit(1);
}

// ===================================================================
// LÓGICA DO RECEPTOR (CLIENTE DOWNLOAD)
// ===================================================================
void run_sr_receiver(int sock, const char* filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) die("fopen receiver");

    struct sockaddr_in remote_addr;
    socklen_t remote_len = sizeof(remote_addr);
    int first_packet = 1;

    int base = 0; 
    ReceiverSlot recv_buffer[WINDOW_SIZE]; 
    memset(recv_buffer, 0, sizeof(recv_buffer));

    printf("[Client-Receiver] Aguardando dados do servidor...\n");

    DataPacket packet;
    AckPacket ack;

    while (1) {
        // Recebe dados
        int recv_len = recvfrom(sock, &packet, sizeof(DataPacket), 0, (struct sockaddr *)&remote_addr, &remote_len);
        
        if (recv_len > 0) {
            if (first_packet) {
                printf("[Client-Receiver] Conexão estabelecida com thread do servidor em %d\n", ntohs(remote_addr.sin_port));
                first_packet = 0;
            }

            // Tratamento do FIN
            if (packet.data_len == 0) {
                printf("[Client-Receiver] Recebeu FIN. Encerrando download.\n");
                ack.ack_num = packet.seq_num;
                for(int k=0; k<5; k++) sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
                break;
            }

            // Envia ACK INDIVIDUAL
            ack.ack_num = packet.seq_num;
            sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);

            // Lógica Selective Repeat (Buffering)
            if (packet.seq_num >= base && packet.seq_num < base + WINDOW_SIZE) {
                int idx = packet.seq_num % WINDOW_SIZE;
                
                if (!recv_buffer[idx].received) {
                    recv_buffer[idx].packet = packet;
                    recv_buffer[idx].received = 1;
                }

                // Desliza a janela e escreve
                while (recv_buffer[base % WINDOW_SIZE].received) {
                    int curr_idx = base % WINDOW_SIZE;
                    fwrite(recv_buffer[curr_idx].packet.data, 1, recv_buffer[curr_idx].packet.data_len, fp);
                    recv_buffer[curr_idx].received = 0;
                    base++;
                    printf("[Client-Receiver] Processou seq=%d. Nova base=%d\n", base-1, base);
                }
            }
        }
    }
    fclose(fp);
    printf("[Client-Receiver] Arquivo '%s' salvo com sucesso.\n", filename);
}

// ===================================================================
// LÓGICA DO EMISSOR (CLIENTE UPLOAD)
// ===================================================================

typedef struct {
    SenderSlot slots[WINDOW_SIZE];
    int base;
    int next_seq_num;
    int finished_reading;
    pthread_mutex_t lock;
    int sock;
    struct sockaddr_in dest_addr;
    socklen_t dest_len;
} SenderState;

// Thread auxiliar para ACKs
void *ack_listener_thread(void *arg) {
    SenderState *state = (SenderState *)arg;
    AckPacket ack;

    while (1) {
        if (recvfrom(state->sock, &ack, sizeof(AckPacket), 0, NULL, NULL) > 0) {
            pthread_mutex_lock(&state->lock);
            
            if (ack.ack_num >= state->base && ack.ack_num < state->base + WINDOW_SIZE) {
                int idx = ack.ack_num % WINDOW_SIZE;
                if (state->slots[idx].is_used && !state->slots[idx].is_acked) {
                    state->slots[idx].is_acked = 1;
                    
                    while (state->slots[state->base % WINDOW_SIZE].is_acked && state->slots[state->base % WINDOW_SIZE].is_used) {
                        state->slots[state->base % WINDOW_SIZE].is_used = 0;
                        state->slots[state->base % WINDOW_SIZE].is_acked = 0;
                        state->base++;
                        printf("[Client-Sender] Janela deslizou para base=%d\n", state->base);
                    }
                }
            }
            if (state->finished_reading && state->base == state->next_seq_num) {
                pthread_mutex_unlock(&state->lock);
                break;
            }
            pthread_mutex_unlock(&state->lock);
        }
    }
    return NULL;
}

// Thread auxiliar para Timeouts Individuais
void *timeout_monitor_thread(void *arg) {
    SenderState *state = (SenderState *)arg;
    while (1) {
        pthread_mutex_lock(&state->lock);
        
        if (state->finished_reading && state->base == state->next_seq_num) {
            pthread_mutex_unlock(&state->lock);
            break;
        }

        time_t now = time(NULL);
        for (int i = state->base; i < state->next_seq_num; i++) {
            int idx = i % WINDOW_SIZE;
            if (state->slots[idx].is_used && !state->slots[idx].is_acked) {
                if (difftime(now, state->slots[idx].time_sent) >= TIMEOUT_SEC) {
                    printf("[Client-Timeout] Reenviando APENAS seq=%d\n", i);
                    sendto(state->sock, &state->slots[idx].packet, sizeof(DataPacket), 0, 
                           (struct sockaddr *)&state->dest_addr, state->dest_len);
                    state->slots[idx].time_sent = now;
                }
            }
        }
        pthread_mutex_unlock(&state->lock);
        usleep(100000); 
    }
    return NULL;
}

void run_sr_sender(int sock, const char* filename, struct sockaddr_in dest_addr) {
    SenderState state;
    memset(&state, 0, sizeof(state));
    state.sock = sock;
    state.dest_addr = dest_addr;
    state.dest_len = sizeof(dest_addr);
    pthread_mutex_init(&state.lock, NULL);

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("Erro: Arquivo local '%s' não encontrado.\n", filename);
        return;
    }

    // Handshake: Espera ACK inicial do servidor para confirmar que ele está pronto e pegar a porta da thread
    AckPacket ack;
    struct sockaddr_in thread_addr;
    socklen_t t_len = sizeof(thread_addr);
    
    printf("[Client-Sender] Aguardando confirmação do servidor para iniciar upload...\n");
    recvfrom(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&thread_addr, &t_len);
    
    // Atualiza o endereço de destino para a porta da thread do servidor
    state.dest_addr = thread_addr;
    printf("[Client-Sender] Servidor pronto na porta %d. Iniciando envio.\n", ntohs(thread_addr.sin_port));

    pthread_t t_ack, t_timer;
    pthread_create(&t_ack, NULL, ack_listener_thread, &state);
    pthread_create(&t_timer, NULL, timeout_monitor_thread, &state);

    char buf[MAX_PAYLOAD];
    size_t bytes_read;

    while (!feof(fp)) {
        pthread_mutex_lock(&state.lock);
        if (state.next_seq_num < state.base + WINDOW_SIZE) {
            bytes_read = fread(buf, 1, MAX_PAYLOAD, fp);
            if (bytes_read > 0) {
                int idx = state.next_seq_num % WINDOW_SIZE;
                state.slots[idx].packet.seq_num = state.next_seq_num;
                state.slots[idx].packet.data_len = bytes_read;
                memcpy(state.slots[idx].packet.data, buf, bytes_read);
                state.slots[idx].is_used = 1;
                state.slots[idx].is_acked = 0;
                state.slots[idx].time_sent = time(NULL);

                printf("[Client-Sender] Enviando seq=%d\n", state.next_seq_num);
                sendto(sock, &state.slots[idx].packet, sizeof(DataPacket), 0, (struct sockaddr *)&state.dest_addr, state.dest_len);
                state.next_seq_num++;
            }
        }
        pthread_mutex_unlock(&state.lock);
        if (state.next_seq_num >= state.base + WINDOW_SIZE) usleep(1000);
    }

    pthread_mutex_lock(&state.lock);
    state.finished_reading = 1;
    pthread_mutex_unlock(&state.lock);

    pthread_join(t_ack, NULL);
    pthread_join(t_timer, NULL);

    DataPacket fin_pkt = { .seq_num = state.next_seq_num, .data_len = 0 };
    for(int i=0; i<3; i++) sendto(sock, &fin_pkt, sizeof(DataPacket), 0, (struct sockaddr *)&state.dest_addr, state.dest_len);

    printf("[Client-Sender] Upload concluído.\n");
    fclose(fp);
}

// ===================================================================
// MAIN
// ===================================================================

int main(int argc, char *argv[]) {
    // MODIFICAÇÃO: Reduzido o número de argumentos necessários para 3
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <upload|download> <arquivo>\n", argv[0]);
        exit(1);
    }
    
    // MODIFICAÇÃO: IP fixo em loopback
    char* server_ip = "127.0.0.1";
    char* op_str = argv[1];
    char* filename = argv[2];

    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) die("socket");

    struct sockaddr_in si_server;
    memset(&si_server, 0, sizeof(si_server));
    si_server.sin_family = AF_INET;
    si_server.sin_port = htons(SERVER_PORT);
    inet_aton(server_ip, &si_server.sin_addr);

    // 1. Envia Requisição
    RequestPacket req;
    strncpy(req.filename, filename, 255);
    if (strcmp(op_str, "upload") == 0) req.operation = OP_UPLOAD;
    else if (strcmp(op_str, "download") == 0) req.operation = OP_DOWNLOAD;
    else {
        fprintf(stderr, "Operação desconhecida. Use 'upload' ou 'download'.\n");
        exit(1);
    }

    sendto(sock, &req, sizeof(req), 0, (struct sockaddr *)&si_server, sizeof(si_server));
    printf("Requisição enviada para %s:9999\n", server_ip);

    // 2. Executa Lógica SR
    if (req.operation == OP_UPLOAD) {
        // Upload = Client é Sender
        run_sr_sender(sock, filename, si_server);
    } else {
        // Download = Client é Receiver
        // Salva com prefixo 'recv_' para não sobrescrever local se estiver testando na mesma pasta
        char out_name[300];
        snprintf(out_name, 300, "%s", filename);
        run_sr_receiver(sock, out_name);
    }

    close(sock);
    return 0;
}
