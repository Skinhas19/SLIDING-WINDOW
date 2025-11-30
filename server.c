#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define PORT 9999
#define WINDOW_SIZE 5
#define TIMEOUT_SEC 1
#define MAX_PAYLOAD 1016 

// --- Estruturas ---

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

// Slot da Janela de Transmissão
typedef struct {
    DataPacket packet;
    int is_acked;       
    int is_used;        
    time_t time_sent;   
} SenderSlot;

// Slot do Buffer de Recepção
typedef struct {
    DataPacket packet;
    int received;       
} ReceiverSlot;

void die(char *s) {
    perror(s);
    exit(1);
}

// ===================================================================
// LÓGICA DO RECEPTOR (Server receiving Upload)
// ===================================================================
void run_sr_receiver(int sock, const char* filename, struct sockaddr_in remote_addr, socklen_t remote_len) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) die("fopen");

    int base = 0; 
    ReceiverSlot recv_buffer[WINDOW_SIZE]; 
    memset(recv_buffer, 0, sizeof(recv_buffer));

    // Envia ACK inicial para destravar o cliente
    AckPacket ack = { .ack_num = -1 }; 
    sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);

    DataPacket packet;
    printf("[Receiver] Aguardando dados...\n");

    while (1) {
        if (recvfrom(sock, &packet, sizeof(DataPacket), 0, (struct sockaddr *)&remote_addr, &remote_len) > 0) {
            
            // Tratamento do FIN
            if (packet.data_len == 0) {
                printf("[Receiver] Recebeu FIN (seq=%d). Encerrando.\n", packet.seq_num);
                ack.ack_num = packet.seq_num;
                // Envia várias vezes para garantir que o cliente receba
                for(int k=0; k<5; k++) sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
                break;
            }

            // Envia ACK do pacote recebido
            ack.ack_num = packet.seq_num;
            sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);

            // Se o pacote está dentro da janela
            if (packet.seq_num >= base && packet.seq_num < base + WINDOW_SIZE) {
                int idx = packet.seq_num % WINDOW_SIZE;
                
                // Se ainda não tínhamos recebido este pacote
                if (!recv_buffer[idx].received) {
                    printf("[Receiver] Recebeu seq=%d (Bufferizado)\n", packet.seq_num); // <--- PRINT ADICIONADO
                    recv_buffer[idx].packet = packet;
                    recv_buffer[idx].received = 1;
                } else {
                     printf("[Receiver] Recebeu duplicata seq=%d (Ignorado)\n", packet.seq_num); // <--- PRINT ADICIONADO
                }

                // Tenta escrever no disco e deslizar a janela
                while (recv_buffer[base % WINDOW_SIZE].received) {
                    int curr_idx = base % WINDOW_SIZE;
                    fwrite(recv_buffer[curr_idx].packet.data, 1, recv_buffer[curr_idx].packet.data_len, fp);
                    
                    printf("[Receiver] Escreveu seq=%d no disco. Nova base=%d\n", base, base+1); // <--- PRINT ADICIONADO
                    
                    recv_buffer[curr_idx].received = 0; // Limpa o slot
                    base++;
                }
            } else {
                printf("[Receiver] Recebeu seq=%d fora da janela (Base atual: %d)\n", packet.seq_num, base);
            }
        }
    }
    fclose(fp);
    printf("[Receiver] Arquivo '%s' salvo com sucesso.\n", filename);
}
// ===================================================================
// LÓGICA DO EMISSOR (Server sending Download)
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
                        printf("[Ack-Listener] Janela deslizou. Nova base=%d\n", state->base);
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
                    printf("[Timeout] Estourou seq=%d. Retransmitindo.\n", i);
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

// ADICIONADO: Parâmetro wait_handshake
void run_sr_sender(int sock, const char* filename, struct sockaddr_in dest_addr, socklen_t dest_len, int wait_handshake) {
    SenderState state;
    memset(&state, 0, sizeof(state));
    state.sock = sock;
    state.dest_addr = dest_addr;
    state.dest_len = dest_len;
    pthread_mutex_init(&state.lock, NULL);

    // CORREÇÃO AQUI: O Servidor NÃO deve esperar handshake no download
    if (wait_handshake) {
        AckPacket ack;
        printf("[Sender] Aguardando receptor (Handshake)...\n");
        recvfrom(sock, &ack, sizeof(AckPacket), 0, NULL, NULL);
    } else {
        printf("[Sender] Iniciando envio IMEDIATAMENTE (Sem Handshake)\n");
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("Arquivo não encontrado no servidor.\n");
        return; 
    }

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

                printf("[Sender] Enviando seq=%d\n", state.next_seq_num);
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
    
    printf("[Sender] Envio concluído.\n");
    fclose(fp);
}

// ===================================================================
// MAIN
// ===================================================================

typedef struct {
    struct sockaddr_in client_addr;
    socklen_t client_len;
    RequestPacket request;
} ClientArgs;

void *handle_client(void *args) {
    ClientArgs *c_args = (ClientArgs *)args;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    
    printf("[Thread] Iniciada para %s\n", c_args->request.filename);

    if (c_args->request.operation == OP_UPLOAD) {
        // Upload: Servidor Recebe
        run_sr_receiver(s, c_args->request.filename, c_args->client_addr, c_args->client_len);
    } else {
        // Download: Servidor Envia
        // CORREÇÃO: wait_handshake = 0 (Não espera, manda logo)
        run_sr_sender(s, c_args->request.filename, c_args->client_addr, c_args->client_len, 0);
    }
    
    close(s);
    free(c_args);
    return NULL;
}

int main() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY };
    
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1) die("bind");
    
    printf("Servidor ouvindo na porta %d\n", PORT);

    while(1) {
        ClientArgs *args = malloc(sizeof(ClientArgs));
        args->client_len = sizeof(args->client_addr);
        if (recvfrom(s, &args->request, sizeof(RequestPacket), 0, (struct sockaddr*)&args->client_addr, &args->client_len) > 0) {
            printf("Recebida conexão de %s:%d\n", inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));
            pthread_t t;
            pthread_create(&t, NULL, handle_client, args);
            pthread_detach(t);
        }
    }
    return 0;
}
