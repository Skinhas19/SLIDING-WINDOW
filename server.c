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
#define TIMEOUT_MS 500
#define MAX_PACKETS 65536
#define MAX_PAYLOAD 1016 // 1024 - 8 bytes (seq_num + data_len)

// Tipos de Operação
#define OP_UPLOAD 1
#define OP_DOWNLOAD 2

// Pacote de Requisição (Cliente -> Servidor)
typedef struct {
    int operation; // OP_UPLOAD ou OP_DOWNLOAD
    char filename[256];
} RequestPacket;

// Pacote de Dados
typedef struct {
    int seq_num;
    int data_len;
    char data[MAX_PAYLOAD];
} DataPacket;

// Pacote de ACK
typedef struct {
    int ack_num; // ACK cumulativo: "Estou esperando o seq_num = ack_num"
} AckPacket;

// Função de utilidade
void die(char *s) {
    perror(s);
    exit(1);
}

// ===================================================================
//
// LÓGICA DO RECEPTOR GBN (GBN Receiver)
//
// ===================================================================
static inline void run_gbn_receiver(int sock, const char* filename, 
                                    struct sockaddr_in remote_addr, socklen_t remote_len,
                                    int send_first_ack) 
{
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) die("fopen (receiver)");

    int expected_seq = 0;
    DataPacket packet;
    AckPacket ack;
    
    // Para UPLOAD, o receptor (servidor) envia o primeiro ACK=0
    if (send_first_ack) {
        printf("[Receiver] Enviando ACK=0 inicial para %s:%d\n",
               inet_ntoa(remote_addr.sin_addr), ntohs(remote_addr.sin_port));
        ack.ack_num = 0;
        sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
    }

    // Loop de recebimento GBN
    while (1) {
        int recv_len = recvfrom(sock, &packet, sizeof(DataPacket), 0, 
                               (struct sockaddr *)&remote_addr, &remote_len);
        
        if (recv_len > 0) {
            // Se esta é a primeira vez que recebemos (para Download),
            // capturamos o endereço da thread do servidor.
            // (Não é estritamente necessário aqui, mas boa prática)

            // Chegou o pacote esperado
            if (packet.seq_num == expected_seq) {
                // Pacote FIN (data_len=0)
                if (packet.data_len == 0) {
                    printf("[Receiver] Recebeu FIN (seq=%d). Enviando ACK final.\n", packet.seq_num);
                    ack.ack_num = expected_seq + 1;
                    sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
                    break; // Termina
                }

                // Pacote de dados normal
                printf("[Receiver] Recebeu seq=%d (OK). Enviando ACK=%d\n", 
                       packet.seq_num, expected_seq + 1);
                
                fwrite(packet.data, 1, packet.data_len, fp);
                
                ack.ack_num = expected_seq + 1;
                sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
                expected_seq++;
            
            } else {
                // Pacote fora de ordem (duplicado ou adiantado)
                printf("[Receiver] Descartou seq=%d (Esperava %d). Reenviando ACK=%d\n", 
                       packet.seq_num, expected_seq, expected_seq);
                
                ack.ack_num = expected_seq;
                sendto(sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&remote_addr, remote_len);
            }
        }
    }

    fclose(fp);
    printf("[Receiver] Transferência concluída para '%s'.\n", filename);
}


// ===================================================================
//
// LÓGICA DO REMETENTE GBN (GBN Sender)
//
// ===================================================================

// Estado compartilhado entre as threads do remetente
typedef struct {
    pthread_mutex_t lock;
    int window_base;
    int next_seq_num;
    int eof_sent;
    int all_acked;
    DataPacket* packet_buffer[MAX_PACKETS];
    int sock;
    struct sockaddr_in dest_addr;
    socklen_t dest_len;
} GbnSenderState;

// Thread que Ouve por ACKs e gerencia o Timeout
static inline void *gbn_ack_listener(void *args) {
    GbnSenderState *state = (GbnSenderState *)args;

    // Configura o timeout no socket
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    if (setsockopt(state->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        die("setsockopt (timeout)");
    }

    AckPacket ack;
    
    while (!state->all_acked) {
        if (recvfrom(state->sock, &ack, sizeof(AckPacket), 0, (struct sockaddr *)&state->dest_addr, &state->dest_len) > 0) {
            
            printf("[Sender-ACK] Recebeu ACK=%d\n", ack.ack_num);

            if (ack.ack_num > state->window_base) {
                pthread_mutex_lock(&state->lock);
                int old_base = state->window_base;
                state->window_base = ack.ack_num; // Desliza a janela
                
                if (state->eof_sent && state->window_base == state->next_seq_num) {
                    state->all_acked = 1;
                }
                pthread_mutex_unlock(&state->lock);

                for (int i = old_base; i < state->window_base; i++) {
                    if (state->packet_buffer[i]) free(state->packet_buffer[i]);
                    state->packet_buffer[i] = NULL;
                }
            }

        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // TIMEOUT!
            pthread_mutex_lock(&state->lock);
            printf("[Sender-ACK] >>> TIMEOUT! Reenviando janela de %d até %d\n", state->window_base, state->next_seq_num - 1);
            
            for (int i = state->window_base; i < state->next_seq_num; i++) {
                sendto(state->sock, state->packet_buffer[i], sizeof(DataPacket), 0, 
                       (struct sockaddr *)&state->dest_addr, state->dest_len);
            }
            pthread_mutex_unlock(&state->lock);
        } else {
            die("recvfrom_listener");
        }
    }
    printf("[Sender-ACK] Listener encerrando.\n");
    pthread_exit(NULL);
}

// Função principal do Remetente
static inline void run_gbn_sender(int sock, const char* filename, 
                                  struct sockaddr_in dest_addr, socklen_t dest_len,
                                  int wait_for_first_ack) 
{
    GbnSenderState state;
    memset(&state, 0, sizeof(GbnSenderState));
    state.sock = sock;
    state.dest_addr = dest_addr;
    state.dest_len = dest_len;

    // Para UPLOAD, o remetente (cliente) espera o ACK=0 do servidor
    if (wait_for_first_ack) {
        AckPacket first_ack;
        printf("[Sender] Aguardando ACK=0 do receptor...\n");
        if (recvfrom(sock, &first_ack, sizeof(AckPacket), 0, (struct sockaddr *)&state.dest_addr, &state.dest_len) > 0) {
            if (first_ack.ack_num == 0) {
                printf("[Sender] Receptor pronto na porta %d. Iniciando envio.\n", ntohs(state.dest_addr.sin_port));
            } else {
                die("Recebido ACK inicial inesperado");
            }
        } else {
            die("Receptor não respondeu (ACK=0)");
        }
    }
    // Para DOWNLOAD, o remetente (servidor) apenas começa a enviar.

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
         printf("[Sender] Erro: Arquivo '%s' não encontrado.\n", filename);
         // (Poderíamos enviar um pacote de erro, mas vamos simplificar e fechar)
         return;
    }
    
    pthread_mutex_init(&state.lock, NULL);
    
    pthread_t ack_tid;
    if (pthread_create(&ack_tid, NULL, gbn_ack_listener, (void*)&state) != 0) {
        die("pthread_create (listener)");
    }

    char read_buffer[MAX_PAYLOAD];
    size_t bytes_read;

    while (1) {
        pthread_mutex_lock(&state.lock);
        
        int window_is_full = (state.next_seq_num >= state.window_base + WINDOW_SIZE);
        
        if (!window_is_full && !state.eof_sent) {
            bytes_read = fread(read_buffer, 1, MAX_PAYLOAD, fp);

            DataPacket *packet = (DataPacket *)malloc(sizeof(DataPacket));
            if (packet == NULL) die("malloc packet");

            packet->seq_num = state.next_seq_num;
            packet->data_len = bytes_read;
            
            if (bytes_read > 0) {
                memcpy(packet->data, read_buffer, bytes_read);
            } else {
                printf("[Sender] Fim do arquivo. Marcando pacote FIN (seq=%d)\n", state.next_seq_num);
                state.eof_sent = 1;
            }

            state.packet_buffer[state.next_seq_num] = packet;
            
            printf("[Sender] Enviando seq=%d (base=%d)\n", state.next_seq_num, state.window_base);
            sendto(state.sock, packet, sizeof(DataPacket), 0, (struct sockaddr *)&state.dest_addr, state.dest_len);
            
            state.next_seq_num++;
            
            pthread_mutex_unlock(&state.lock);

        } else {
            pthread_mutex_unlock(&state.lock);
            if (state.all_acked) {
                break;
            }
            usleep(10000); // Evita busy-waiting
        }
    }

    pthread_join(ack_tid, NULL);

    printf("[Sender] Envio de '%s' concluído.\n", filename);
    fclose(fp);
    pthread_mutex_destroy(&state.lock);

    // Limpa qualquer pacote restante no buffer (em caso de saída antecipada)
    for (int i = state.window_base; i < state.next_seq_num; i++) {
        if(state.packet_buffer[i]) free(state.packet_buffer[i]);
    }
}

// Estrutura para passar dados para a thread do cliente
typedef struct {
    struct sockaddr_in client_addr;
    socklen_t client_len;
    RequestPacket request; // A requisição original
} ClientThreadArgs;

// Função da thread que lida com UM cliente
void *handle_client(void *args) {
    ClientThreadArgs *t_args = (ClientThreadArgs *)args;
    struct sockaddr_in client_addr = t_args->client_addr;
    socklen_t client_len = t_args->client_len;
    
    // 1. Criar um NOVO socket para esta thread
    int thread_sock;
    if ((thread_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("thread socket");
    }

    printf("[Thread %ld] Iniciada para %s:%d (Op: %d, Arq: %s)\n", 
           pthread_self(), inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
           t_args->request.operation, t_args->request.filename);
    
    // 2. Decidir qual lógica rodar
    if (t_args->request.operation == OP_UPLOAD) {
        // Cliente quer enviar, servidor é o RECEPTOR
        // Receptor GBN envia o primeiro ACK=0
        run_gbn_receiver(thread_sock, t_args->request.filename, 
                         client_addr, client_len, 
                         1); // 1 = true (send_first_ack)

    } else if (t_args->request.operation == OP_DOWNLOAD) {
        // Cliente quer baixar, servidor é o REMETENTE
        // Remetente GBN *não* espera por ACK=0, apenas começa a enviar
        run_gbn_sender(thread_sock, t_args->request.filename, 
                       client_addr, client_len, 
                       0); // 0 = false (wait_for_first_ack)
    }

    close(thread_sock);
    printf("[Thread %ld] Encerrando.\n", pthread_self());
    free(t_args);
    pthread_exit(NULL);
}

// Thread principal do servidor (Ouve por novas conexões)
int main(void) {
    struct sockaddr_in si_me, si_other;
    int s;
    socklen_t slen = sizeof(si_other);

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) die("socket");

    memset((char *)&si_me, 0, sizeof(si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&si_me, sizeof(si_me)) == -1) die("bind");

    printf("Servidor GBN (Upload/Download) ouvindo na porta %d...\n", PORT);

    while (1) {
        ClientThreadArgs *args = (ClientThreadArgs *)malloc(sizeof(ClientThreadArgs));
        if (args == NULL) die("malloc");

        args->client_len = slen;

        // 1. Espera por um pacote de REQUISIÇÃO de um novo cliente
        if (recvfrom(s, &(args->request), sizeof(RequestPacket), 0, 
                    (struct sockaddr *)&(args->client_addr), &(args->client_len)) > 0) {
            
            printf("Recebida requisição de %s:%d. Criando thread...\n",
                   inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));

            // 2. Cria a thread para cuidar desse cliente
            pthread_t tid;
            if (pthread_create(&tid, NULL, handle_client, (void *)args) != 0) {
                die("pthread_create");
            }
            pthread_detach(tid); 
        } else {
            free(args);
        }
    }
    close(s);
    return 0;
}
