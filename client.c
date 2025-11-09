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

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <upload|download> <nome_do_arquivo>\n", argv[0]);
        exit(1);
    }
    
    char* operation_str = argv[1];
    char* filename = argv[2];
    char* server_ip = "127.0.0.1";

    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) die("socket");

    // Endereço do LISTENER PRINCIPAL do servidor (Porta 9999)
    struct sockaddr_in si_server_main;
    memset((char *)&si_server_main, 0, sizeof(si_server_main));
    si_server_main.sin_family = AF_INET;
    si_server_main.sin_port = htons(PORT);
    inet_aton(server_ip, &si_server_main.sin_addr);

    // 1. Prepara e envia o pacote de requisição
    RequestPacket req;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.filename[sizeof(req.filename) - 1] = '\0'; // Garante terminação nula

    if (strcmp(operation_str, "upload") == 0) {
        req.operation = OP_UPLOAD;
    } else if (strcmp(operation_str, "download") == 0) {
        req.operation = OP_DOWNLOAD;
    } else {
        fprintf(stderr, "Operação inválida: use 'upload' ou 'download'\n");
        exit(1);
    }

    sendto(sock, &req, sizeof(RequestPacket), 0, (struct sockaddr *)&si_server_main, sizeof(si_server_main));
    printf("Requisição de %s para o arquivo '%s' enviada ao servidor.\n", operation_str, filename);

    // O endereço 'si_server_thread' será preenchido pelo primeiro
    // pacote recebido da thread dedicada do servidor.
    struct sockaddr_in si_server_thread;
    socklen_t slen = sizeof(si_server_thread);
    
    // Copiamos os detalhes do servidor principal, mas a porta será
    // sobrescrita por recvfrom()
    memcpy(&si_server_thread, &si_server_main, sizeof(si_server_main));


    // 2. Decidir qual lógica rodar
    if (req.operation == OP_UPLOAD) {
        // Cliente é o REMETENTE
        // Remetente GBN *espera* pelo ACK=0 (que virá da nova porta da thread)
        run_gbn_sender(sock, filename, 
                       (struct sockaddr_in)si_server_thread, slen, // Endereço será preenchido por recvfrom dentro da função
                       1); // 1 = true (wait_for_first_ack)
    
    } else if (req.operation == OP_DOWNLOAD) {
        // Cliente é o RECEPTOR
        char local_filename[300];
        snprintf(local_filename, sizeof(local_filename), "%s", filename);

        // Receptor GBN *não* envia ACK=0, apenas espera pelo pacote 0
        run_gbn_receiver(sock, local_filename, 
                         (struct sockaddr_in)si_server_thread, slen, // Endereço será preenchido por recvfrom dentro da função
                         0); // 0 = false (send_first_ack)
    }

    close(sock);
    return 0;
}
