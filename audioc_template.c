/*

COMPILAR Y EJECUTAR EL CÓDICGO
gcc -Wall -g -o audioc audioc_template.c ../lib/configureSndcard.c ../lib/circularBuffer.c ../lib/rtp.h audiocArgs.c
./audioc 239.3.4.5 1 -c
strace -o Traza ./audioc 239.3.4.5 1 -c
strace -tt -o strace_Gonzalez_Sanz_300 ./audioc 239.3.4.5 1 -k300 -c > verbose_Gonzalez_Sanz_300
strace -tt -o strace_Gonzalez_Sanz_3000 ./audioc 239.3.4.5 1 -k3000-c > verbose_Gonzalez_Sanz_3000
./audiocTest 239.3.4.5 -k3000 -1 -2 -3
rtpdump 239.3.4.5/5004 > rtpdump_Gonzalez_Sanz_3000

*/


#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include <sys/signalfd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>




#include "audiocArgs.h"
#include "../lib/circularBuffer.h"
#include "../lib/configureSndcard.h"
#include "../lib/rtp.h"

void * circularBuffer;
//Función para hacer la resta de tiempos para l reproducción real
float time_diff(struct timeval *start, struct timeval *end){
    return (end->tv_sec - start->tv_sec) + 1e-6*(end->tv_usec - start->tv_usec);
}
int main(int argc, char *argv[]){
    struct in_addr multicastIp;
    uint32_t ssrc;
    uint16_t port;
    uint32_t packetDuration; /* in milliseconds */
    uint32_t bufferingTime; /* in milliseconds */
    bool verbose;
    uint8_t payload;

    int channelNumber;
    int sndCardFormat;
    int rate;

    int requestedFragmentSize;
    int numberOfBlocks;
    int sndCardDesc;

    struct sockaddr_in remToSendSAddr;
	struct sockaddr_in remToSendSAddr_recive;
    struct ip_mreq mreq;
    int socketDesc;

    /* Put stdout in unbuffered mode, so that it prints immediately, without waiting for '\n' */
    if (setvbuf(stdout, NULL, _IONBF, 0) < 0) {
        perror("setvbuf");
        exit(EXIT_FAILURE);
    }

    /* Obtains values from the command line - or default values otherwise */
    if (args_capture_audioc(argc, argv, &multicastIp, &ssrc,
            &port, &packetDuration, &verbose, &payload, &bufferingTime) == EXIT_FAILURE)
    { exit(EXIT_FAILURE);  /* there was an error parsing the arguments, error info
                   is printed by the args_capture function */
    }

    /*************************/
    /* gets signal_fd and blocks signals, so that they will be processed inside the select */
    sigset_t sigInfo;
    sigemptyset(&sigInfo);
    sigaddset(&sigInfo, SIGINT);
    sigaddset(&sigInfo, SIGALRM);
    sigaddset(&sigInfo, SIGTERM);
    int signal_fd = signalfd(-1, &sigInfo, 0);
    if (signal_fd < 0) {
        printf("Error getting file descriptor for signals, error: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // block SIGINT, SIGALRM and SIGTERM signals with sigprocmask
    if (sigprocmask(SIG_BLOCK, &sigInfo, NULL) < 0) {
        printf("Error installing signal, error: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /*************************/
    /* Configures sound card */
    channelNumber = 1;
	  int Bytes_per_sample = 0;
    if (payload == PCMU) {
        rate = 8000;
        sndCardFormat = AFMT_MU_LAW;
		Bytes_per_sample = 1;
    } else if (payload == L16_1) {
        rate = 8000;
        sndCardFormat = AFMT_S16_BE;
		Bytes_per_sample = 2;
    }

	//l(ms)*Bytes(muestras/s)*rate(s/muestra)
    requestedFragmentSize = (packetDuration*Bytes_per_sample*rate)*pow(10,-3);
	//(bytes/paq)
    
	/* configures sound card, sndCardDesc is filled after it returns */
    configSndcard (&sndCardDesc, &sndCardFormat, &channelNumber, &rate, &requestedFragmentSize, true);


    /*************************/
    /* Creates circular buffer */
    numberOfBlocks = (((bufferingTime+200)*Bytes_per_sample*rate)/requestedFragmentSize)*pow(10,-3);
    circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);


    /*************************/
    /* Configures socket */
    bzero(&remToSendSAddr, sizeof(remToSendSAddr));
    remToSendSAddr.sin_family = AF_INET;
    remToSendSAddr.sin_port = htons(port);
    remToSendSAddr.sin_addr = multicastIp;

    /* Creates socket */
    if ((socketDesc = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("socket error\n");
        exit(EXIT_FAILURE);
    }

    /* configure SO_REUSEADDR, multiple instances can bind to the same multicast address/port */
    int enable = 1;
    if (setsockopt(socketDesc, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        printf("setsockopt(SO_REUSEADDR) failed");
        exit(EXIT_FAILURE);
    }

    if (bind(socketDesc, (struct sockaddr *)&remToSendSAddr, sizeof(struct sockaddr_in)) < 0) {
        printf("bind error\n");
        exit(EXIT_FAILURE);
    }

    /* setsockopt configuration for joining to mcast group */
    mreq.imr_multiaddr = multicastIp;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socketDesc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        printf("setsockopt error");
        exit(EXIT_FAILURE);
    }

    /* Do not receive packets sent to the mcast address by this process */
    unsigned char loop=0;
    setsockopt(socketDesc, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(unsigned char));


    /* FILL:
    - Record one audio packet and sent it through the socket. Generate RTP info.
    - 'while' to accumulate bufferingTime data, with select that coordinates:
        exit when Ctrl-C is pressed.
        audio recording + send packet
        receive from socket + store in circular buffer
    - When we have enough data, 'while' that also plays from circular buffer.

    Then:
    - add silence processing
    - check for packets lost (only after 10 s have elapsed)
    - ...
    */

	int bytesRead;
	int bytesWrite;
	int result;

	//PAQUETE = cabecera+datosAudio
	uint8_t paquete[12+requestedFragmentSize];
	rtp_hdr_t * cabeceraRTP;
	int8_t * datosAudio;
	cabeceraRTP = (rtp_hdr_t *) paquete;
    datosAudio = (int8_t *) (cabeceraRTP + 1);

	//Inicializar cabecera
	unsigned int seq_send=0;
    u_int32 ts_send=0;
	unsigned int seq_recv=0;
    u_int32 ts_recv=0;
	(*cabeceraRTP).version = 2;
	cabeceraRTP->p = 0;
	cabeceraRTP->x = 0;
	cabeceraRTP->cc = 0;
	cabeceraRTP->m = 0;
	cabeceraRTP->pt = payload;
	cabeceraRTP->seq =0;
	cabeceraRTP->ts = 0;
	cabeceraRTP->ssrc = htonl(ssrc);


    //Crear paquete de silencio con ruido
	uint8_t silence_sequence[] = {0x80,0x77,0x82,0x83,0x84,0x85,0x86,0x77};
	uint8_t silencio[requestedFragmentSize];
	for (int i=0; i < requestedFragmentSize/8; i++) {
		memcpy( silencio , silence_sequence, 8);
	}


	//Tiempos para ver la duración de la reproducción
	struct timeval start;
    struct timeval end;
	

	//Número de paquetes totales recibidos
    int recibido = 0;
	//Número de paquetes enviados
	int enviados = 0;
	//Número de paquetes perdidos
	int perdidos = 0;
	//Número de paquetes de silencio insertados
	int silencios = 0;
	//Número de paquetes correctos insertados
	int correctos = 0;
    //Número de paquetes que hay en el buffer circular
    int paq_bc = 0;
    //Número de paquetes que hay que acumuluar
    int k = ((bufferingTime*Bytes_per_sample*rate)/requestedFragmentSize)*pow(10,-3);
    //Duración de un paquete para el timeStamp
    int time_packet = requestedFragmentSize/Bytes_per_sample;
    //Número de paquetes insertados en la tarjeta de sonido
    int tarjetaSonido = 0;
	//Temporizador para el select
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	//Bytes de la tarjeta de sonido
	int bytes_tj = 0; 
	//Bytes de la tarjeta de sonido y el bc
	int bytes_totales = 0;
	//Tiempo de reproducción que queda en tjs y bc
	int tiempo_total = 0;

	//LEER Y ENVIAR 1 PAQUETE
	if ((bytesRead = read (sndCardDesc, datosAudio, requestedFragmentSize)) < 0){
	    printf("Error reading from soundcard, error: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (bytesRead!= requestedFragmentSize){
		printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
	}
	if ((result = sendto(socketDesc, paquete, sizeof(paquete), /* flags */ 0, (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr)))<0) {
        printf("sendto error\n");
    }
    else {
        if(verbose==true){
            printf(".");
        }
		enviados = enviados + 1;
    }

	//ACUMULAR DATOS EN EL BC
	while(recibido != k ){
		fd_set conjunto_lectura, conjunto_escritura;
		FD_ZERO(&conjunto_lectura);
		FD_SET(socketDesc, &conjunto_lectura);
		FD_SET(sndCardDesc, &conjunto_lectura);
		FD_SET(signal_fd, &conjunto_lectura);
		FD_ZERO(&conjunto_escritura);
		int res = select (FD_SETSIZE, &conjunto_lectura, &conjunto_escritura, NULL,	NULL);
		if (res <0) {
			printf ("Select error\n");
			exit(-1);
		}
		else {
			//ENVIANDO UN PAQUETE
			if (FD_ISSET (sndCardDesc, &conjunto_lectura) == 1){ 
                //Leyendo de la tarjeta de sonido
				if ((bytesRead = read (sndCardDesc, datosAudio, requestedFragmentSize)) < 0){
					printf("Error reading from soundcard, error: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}
				if (bytesRead!= requestedFragmentSize){
					printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
				}
                //Actualizar el número de secuencia y el timeStamp
				seq_send = seq_send + 1;
				ts_send = ts_send + time_packet;
                //Cambiar la cabecera a orden de red
				cabeceraRTP->seq = htons(seq_send);
				cabeceraRTP->ts = htonl(ts_send);
				cabeceraRTP->ssrc = htonl(ssrc);
				cabeceraRTP->pt = payload;
                //Enviar el paquete
				if ((result = sendto(socketDesc, paquete, sizeof(paquete), 0, (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr)))<0) {
					printf("sendto error\n");
				}
				else{
					enviados = enviados + 1;
					if(verbose==true){
						printf(".");
					}
				}
			}
			//RECIBIR UN PAQUETE
			if (FD_ISSET (socketDesc, &conjunto_lectura) == 1){ 
				socklen_t sockAddrInLength = sizeof (struct sockaddr_in);
                //Se guarda el paquete
				if ((result = recvfrom(socketDesc, paquete, sizeof(paquete), 0, (struct sockaddr *) &remToSendSAddr_recive, &sockAddrInLength)) < 0) {
					printf ("recvfrom error\n");
				}
				else {
					if(verbose==true){
						printf("+");
					}
                    //Guardar en el buffer circular el paquete (nunca hay overrun)
					memcpy(cbuf_pointer_to_write(circularBuffer), paquete+12, requestedFragmentSize);
					recibido = recibido + 1;
					paq_bc=paq_bc +1;
					rtp_hdr_t * cabeceraRTP_recibida;
					cabeceraRTP_recibida = (rtp_hdr_t *) paquete;
					seq_recv = ntohs(cabeceraRTP_recibida->seq);
					ts_recv = ntohl(cabeceraRTP_recibida->ts);
				}
			}
			// SE INTRODUCE EL CTRL+C
			if (FD_ISSET (signal_fd, &conjunto_lectura) == 1){ 
				if(circularBuffer!=NULL){
                    cbuf_destroy_buffer(circularBuffer);
                }
                if(verbose==true){
                    printf ("CTRL+C\n");
                    printf("Aún no se ha inicializado la reproducción");
					printf("Número de paquetes enviados: %i\n", enviados);
					printf("Número de paquetes recibidos: %i \n", recibido);
                }
                exit(0);
			}
		}
	}

	//CUANDO YA SE LLENA EL BUFFER HASTA K
	while(1){
	
	//Cáculo del timeout
		//Bytes en la tarjeta de sonido
		if(ioctl(sndCardDesc, SNDCTL_DSP_GETODELAY,&bytes_tj)<0){
        	printf("Ioctl error\n" );
        	exit(-1);
      	}
		//Bytes totales
		bytes_totales = bytes_tj + paq_bc*requestedFragmentSize;
		
		//Tiempo del timer en ms (-10ms de margen)
		tiempo_total = (bytes_totales)/(rate*Bytes_per_sample*pow(10,-3)) - 10;
		int segundos_totales = 0;
		int usegundos_totales = 0;
		if(tiempo_total>=0){
			segundos_totales = tiempo_total*pow(10,-3); 
			usegundos_totales = (tiempo_total-segundos_totales*pow(10,3))*pow(10,3);
		} 		
		timeout.tv_sec = segundos_totales; 
		timeout.tv_usec = usegundos_totales;


    //Si hay paquetes en el buffer circular
		if(paq_bc>0){
			
			fd_set conjunto_lectura, conjunto_escritura;
			FD_ZERO(&conjunto_lectura);
			FD_SET(socketDesc, &conjunto_lectura);
			FD_SET(sndCardDesc, &conjunto_lectura);
			FD_SET(signal_fd, &conjunto_lectura);
			FD_ZERO(&conjunto_escritura);
			FD_SET(sndCardDesc, &conjunto_escritura);

			int res = select (FD_SETSIZE, &conjunto_lectura, &conjunto_escritura, NULL, &timeout);
			if (res <0) {
				printf ("Select error\n");
				exit(-1);
			}
			//SI SE EXPIRA EL TIMER
			else if(res==0){
				if(verbose==true){
					printf("t");
				}
				//Se guarda un paquete de silencio en el buffercircular	
				void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
				if(bloque_escribir!=NULL){
					memcpy(bloque_escribir, silencio, requestedFragmentSize);
					paq_bc=paq_bc +1;
					silencios = silencios + 1;
					ts_recv= ts_recv+time_packet;
				}else{ // si hay overrun eliminar buffer
					if(circularBuffer!=NULL){
						cbuf_destroy_buffer(circularBuffer);
					}
					circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
					paq_bc = 0;
				} 	

			}
			else{
				//REPRODUCIR (GUARDAR EN LA TARJETA DE SONIDO)
				if (FD_ISSET (sndCardDesc, &conjunto_escritura) == 1){
					//Escribir en la tarjeta de sonido
					void *bloque_escribir = cbuf_pointer_to_read(circularBuffer);
					if(bloque_escribir!=NULL){
						if ((bytesWrite = write(sndCardDesc, bloque_escribir, requestedFragmentSize)) < 0){
							printf("Error writing to soundcard, error: %s\n", strerror(errno));
							exit(EXIT_FAILURE);
						}
						if (bytesWrite!= requestedFragmentSize){
							printf ("Played a different number of bytes than expected (played %d bytes, expected %d; exiting)\n", bytesWrite, requestedFragmentSize);
							exit(EXIT_FAILURE);
						}
						tarjetaSonido = tarjetaSonido + 1; 
						paq_bc = paq_bc - 1;
						if(tarjetaSonido==1){ //Si esto ocurre es que se inicia la reproducción
							gettimeofday(&start, NULL); 
						}	
						if(verbose==true){
							printf("-");
						}
					}
					else{ //No se haría nunca
						printf("No hay datos en el buffer circular");
					}
				}
				//ENVIAR PAQUETES	
				if (FD_ISSET (sndCardDesc, &conjunto_lectura) == 1){ 
					//Leer de la tarjeta de sonido
					if ((bytesRead = read (sndCardDesc, datosAudio, requestedFragmentSize)) < 0){
						printf("Error reading from soundcard, error: %s\n", strerror(errno));
						exit(EXIT_FAILURE);
					}
					if (bytesRead!= requestedFragmentSize){
						printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
					}
					//Actualizar las cabeceras
					seq_send = seq_send + 1;
					ts_send = ts_send + time_packet;

					//Cambiar la cabecera orden de red
					cabeceraRTP->seq = htons(seq_send);
					cabeceraRTP->ts = htonl(ts_send);
					cabeceraRTP->ssrc = htonl(ssrc);
					cabeceraRTP->pt = payload;

					//Enviar el paquete
					if ( (result = sendto(socketDesc, paquete, sizeof(paquete), 0, (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr)))<0) {
						printf("sendto error\n");
					}
					else {
						enviados = enviados + 1;
						if(verbose==true){
							printf(".");
						}
					}
				}
				//RECIBIR UN PAQUETE
				if (FD_ISSET (socketDesc, &conjunto_lectura) == 1){
					socklen_t sockAddrInLength = sizeof (struct sockaddr_in);
					if ((result = recvfrom(socketDesc, paquete, sizeof(paquete), 0, (struct sockaddr *) &remToSendSAddr_recive, &sockAddrInLength)) < 0) {
						printf ("recvfrom error\n");
					}
					else { 
						rtp_hdr_t * cabeceraRTP_recibida;
						cabeceraRTP_recibida = (rtp_hdr_t *) paquete;
						//Si se recibe el paquete correcto se guarda directamente
						if((ntohs(cabeceraRTP_recibida->seq) == seq_recv+1) && (ntohl(cabeceraRTP_recibida->ts)==ts_recv+time_packet)){
							void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
							if(bloque_escribir!=NULL){
								if(verbose==true){
									printf("+");
								}
								memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
								recibido = recibido + 1;
								correctos = correctos + 1;
								paq_bc=paq_bc +1;
							}else{ // si hay overrun eliminar buffer
								if(circularBuffer!=NULL){
									cbuf_destroy_buffer(circularBuffer);
								}
								circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
								paq_bc = 0;
							} 
							seq_recv = ntohs(cabeceraRTP_recibida->seq);
							ts_recv = ntohl(cabeceraRTP_recibida->ts);
						}else{ 
							//Si no coincide ni el timestamp ni el número de secuencia
							if((ntohs(cabeceraRTP_recibida->seq) != seq_recv+1)&& (ntohs(cabeceraRTP_recibida->ts)!=ts_recv+time_packet)){
								//Paquetes que se pierden
								int paquetes_perdidos = ntohs(cabeceraRTP_recibida->seq)-seq_recv-1;
								for(int i = 0; i<paquetes_perdidos; i++){
									if(verbose==true){
										printf("x");
									}
									perdidos = perdidos + 1;
								}
								//Insertar silencios por cada paquete
								for(int i = 0; i<paquetes_perdidos; i++){
									void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
									if(bloque_escribir!=NULL){
										memcpy(bloque_escribir, silencio, requestedFragmentSize);
										paq_bc=paq_bc +1;
										if(verbose==true){
											printf("~");
										}
										silencios = silencios + 1;
									}else{ // si hay overrun eliminar buffer
										if(circularBuffer!=NULL){
											cbuf_destroy_buffer(circularBuffer);
										}
										circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
										paq_bc = 0;
									} 	
								}	
								//Escribir el mensaje que se ha recibido			
								void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
								if(bloque_escribir!=NULL){
									memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
									recibido = recibido + 1;
									paq_bc=paq_bc +1;
									correctos = correctos + 1;
									if(verbose==true){
										printf("+");
									}
										
								}else{ // si hay overrun eliminar buffer
									if(circularBuffer!=NULL){
										cbuf_destroy_buffer(circularBuffer);
									}
									circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
									paq_bc = 0;
								} 
								//Actualizar los números de secuencia y timestamp
								seq_recv = ntohs(cabeceraRTP_recibida->seq);
								ts_recv = ntohl(cabeceraRTP_recibida->ts);	
							}
							//Si no coincide el timestap es que el emisor ha insertaro silencios a propósito
						    if((ntohs(cabeceraRTP_recibida->seq) == seq_recv+1)&& ntohs(cabeceraRTP_recibida->ts)!=ts_recv+time_packet){
								int paquetes_silencio = (ntohl(cabeceraRTP_recibida->ts)-ts_recv-time_packet)/time_packet;
								//Insertar silencios al buffercircular
								for(int i = 0; i<paquetes_silencio; i++){
									void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
									if(bloque_escribir!=NULL){
										memcpy(bloque_escribir, silencio, requestedFragmentSize);
										recibido = recibido + 1;
										paq_bc=paq_bc +1;
										if(verbose==true){
											printf("~");
										}
										silencios = silencios + 1;
									}
									else{ // si hay overrun eliminar buffer
										if(circularBuffer!=NULL){
											cbuf_destroy_buffer(circularBuffer);
										}
										circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
										paq_bc = 0;
									} 	
								}
								//Escribir el paquete recibido
								void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
								if(bloque_escribir!=NULL){
									memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
									recibido = recibido + 1;
									paq_bc=paq_bc +1;
									correctos = correctos + 1;
									if(verbose==true){
										printf("+");
									}
								}
								else{ // si hay overrun eliminar buffer
									if(circularBuffer!=NULL){
										cbuf_destroy_buffer(circularBuffer);
									}
									circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
									paq_bc = 0;
								}
								//Actualizar los números de secuencia y timestamp
								seq_recv = ntohs(cabeceraRTP_recibida->seq);
								ts_recv = ntohl(cabeceraRTP_recibida->ts);
							}
						}
					}
					
				}
				// SI SE PULSA EL CTRL+C
				if (FD_ISSET (signal_fd, &conjunto_lectura) == 1){ 
					printf ("CTRL+C\n");
					if(circularBuffer!=NULL){
						cbuf_destroy_buffer(circularBuffer);
					}
					if(verbose==true){
						//Obtener el tiempo real de reproducción
						gettimeofday(&end, NULL);
						printf("Tiempo real desde que se reprodujo el primer paquete: %0.8f sec\n", time_diff(&start, &end));
						//Obtener el tiempo teórico de reproducción
						float tiempo_real = tarjetaSonido*requestedFragmentSize/(Bytes_per_sample*rate);
						printf("Tiempo teórico de reproducción: %lf sec\n", tiempo_real );
						printf("Número de paquetes enviados: %i\n", enviados);
						printf("Número de paquetes recibidos: %i\n", recibido);
						printf("Número de paquetes perdidos: %i\n", perdidos);
						printf("Número de paquetes de silencio insertados: %i\n", silencios);
						printf("Número de paquetes correctos insertados: %i\n", correctos);
					}
					exit(0);
				}
			}

		}
		
    //Si no hay paquetes en el buffer circular
		else{
			fd_set conjunto_lectura, conjunto_escritura;
			FD_ZERO(&conjunto_lectura);
			FD_SET(socketDesc, &conjunto_lectura);
			FD_SET(sndCardDesc, &conjunto_lectura);
			FD_SET(signal_fd, &conjunto_lectura);
			FD_ZERO(&conjunto_escritura);

			int res = select (FD_SETSIZE, &conjunto_lectura, &conjunto_escritura, NULL,	&timeout);
			if (res <0) {
				printf ("Select error\n");
				exit(-1);
			}
			// SI SALTA EL TEMPORIZADOR
			else if(res==0){
				if(verbose==true){
					printf("t");
				}	
				void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
				if(bloque_escribir!=NULL){
					memcpy(bloque_escribir, silencio, requestedFragmentSize);
					paq_bc=paq_bc +1;
					silencios = silencios + 1;
					ts_recv=ts_recv+time_packet;
				}else{ // si hay overrun eliminar buffer
					if(circularBuffer!=NULL){
						cbuf_destroy_buffer(circularBuffer);
					}
					circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
					paq_bc = 0;
				} 		
			}
			else{
                //ENVIAR PAQUETES
				if (FD_ISSET (sndCardDesc, &conjunto_lectura) == 1){
                     //Leyendo de la tarjeta de sonido
					if ((bytesRead = read (sndCardDesc, datosAudio, requestedFragmentSize)) < 0){
					   	printf("Error reading from soundcard, error: %s\n", strerror(errno));
						exit(EXIT_FAILURE);
					}
					if (bytesRead!= requestedFragmentSize){
						printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
					}
                    //Actualizar el número de secuencia y el timeStamp
					seq_send = seq_send + 1;
					ts_send = ts_send + time_packet;

					//Cabecera orden de red
					cabeceraRTP->seq = htons(seq_send);
					cabeceraRTP->ts = htonl(ts_send);
					cabeceraRTP->ssrc = htonl(ssrc);

                    //Enviar el paquete
					if((result = sendto(socketDesc, paquete, sizeof(paquete), /* flags */ 0, (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr)))<0) {
						printf("sendto error\n");
					}
					else {
						enviados = enviados + 1;
						if(verbose==true){
							printf(".");
						}
					}
				}
				//RECIBIR UN PAQUETE
				if (FD_ISSET (socketDesc, &conjunto_lectura) == 1){ 
					socklen_t sockAddrInLength = sizeof (struct sockaddr_in);
					if ((result = recvfrom(socketDesc, paquete, sizeof(paquete), 0, (struct sockaddr *) &remToSendSAddr_recive, &sockAddrInLength)) < 0) {
						printf ("recvfrom error\n");
					}
					else {
                        //Procesar la cabecera
						rtp_hdr_t * cabeceraRTP_recibida;
						cabeceraRTP_recibida = (rtp_hdr_t *) paquete;

                        //Número de secuencia y timeStamp recibido correcto
						if((ntohs(cabeceraRTP_recibida->seq) == seq_recv+1) && (ntohl(cabeceraRTP_recibida->ts)==ts_recv+time_packet)){
							void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
               			 	if(bloque_escribir!=NULL){
								if(verbose==true){
									printf("+");
								}
								memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
								recibido = recibido + 1;
								correctos = correctos + 1;
								paq_bc=paq_bc +1;
							}else{ // si hay overrun eliminar buffer
								if(circularBuffer!=NULL){
									cbuf_destroy_buffer(circularBuffer);
								}
								circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
								paq_bc = 0;
							}			 
							seq_recv = ntohs(cabeceraRTP_recibida->seq);
							ts_recv = ntohl(cabeceraRTP_recibida->ts);	
						}
						else{ 
							//Si no son correcto ni el número de secuencia ni el timestap
                       		if((ntohs(cabeceraRTP_recibida->seq) != seq_recv+1)&& (ntohs(cabeceraRTP_recibida->ts)!=ts_recv+time_packet)){
								int paquetes_perdidos = ntohs(cabeceraRTP_recibida->seq)-seq_recv-1;
								//contar los paquetes perdidos
								for(int i = 0; i<paquetes_perdidos; i++){
									if(verbose==true){
							 			printf("x");
									}
									perdidos = perdidos + 1;

								}
								//Insertar un silencio por cada paquete perdido
								for(int i = 0; i<paquetes_perdidos; i++){
									void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
                					if(bloque_escribir!=NULL){
										memcpy(bloque_escribir, silencio, requestedFragmentSize);
										paq_bc=paq_bc +1;
										if(verbose==true){
											printf("~");
										}
										silencios = silencios + 1;
									}
									else{ // si hay overrun eliminar buffer
										if(circularBuffer!=NULL){
											cbuf_destroy_buffer(circularBuffer);
										}
										circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
										paq_bc = 0;
									} 	
								}
								//Insertar el paquete correcto recibido				
								void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
               					if(bloque_escribir!=NULL){
									memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
									recibido = recibido + 1;
									paq_bc=paq_bc +1;
									correctos = correctos + 1;
									if(verbose==true){
											printf("+");
									}
									
								}else{ // si hay overrun eliminar buffer
									if(circularBuffer!=NULL){
										cbuf_destroy_buffer(circularBuffer);
									}
									circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
									paq_bc = 0;
								}
								//Actualizar los números de secuencia y timestamp
								seq_recv = ntohs(cabeceraRTP_recibida->seq);
								ts_recv = ntohl(cabeceraRTP_recibida->ts);	
							}
							// El emisor introduce un silencio
							if((ntohs(cabeceraRTP_recibida->seq) == seq_recv+1)&& ntohs(cabeceraRTP_recibida->ts)!=ts_recv+time_packet){
								int paquetes_silencio = (ntohl(cabeceraRTP_recibida->ts)-ts_recv-time_packet)/time_packet;
								//Paquetes de silencio a insertar
								for(int i = 0; i<paquetes_silencio; i++){
									void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
                					if(bloque_escribir!=NULL){
										memcpy(bloque_escribir, silencio, requestedFragmentSize);
										recibido = recibido + 1;
										paq_bc=paq_bc +1;
										if(verbose==true){
											printf("~");
										}
										silencios = silencios + 1;
									}
									else{ // si hay overrun eliminar buffer
										if(circularBuffer!=NULL){
											cbuf_destroy_buffer(circularBuffer);
										}
										circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
										paq_bc = 0;
									} 	
								}
								//Escribir en el buffercircular el paquete correcto
								void *bloque_escribir = cbuf_pointer_to_write(circularBuffer);
               					if(bloque_escribir!=NULL){
									memcpy(bloque_escribir, paquete+12, requestedFragmentSize);
									recibido = recibido + 1;
									paq_bc=paq_bc +1;
									correctos = correctos + 1;
									if(verbose==true){
										printf("+");
									}
								}else{ // si hay overrun eliminar buffer
									if(circularBuffer!=NULL){
										cbuf_destroy_buffer(circularBuffer);
									}
									circularBuffer = cbuf_create_buffer(numberOfBlocks, requestedFragmentSize);
									paq_bc = 0;
								} 
								//Actualizar los números de secuencia y timestamp
								seq_recv = ntohs(cabeceraRTP_recibida->seq);
								ts_recv = ntohl(cabeceraRTP_recibida->ts);
							}


						}


					}
				}
				//SI SE PULSA EL CTRL+C
				if (FD_ISSET (signal_fd, &conjunto_lectura) == 1){ 
						printf ("CTRL+C\n");
						if(circularBuffer!=NULL){
							cbuf_destroy_buffer(circularBuffer);
						}
						if(verbose==true){
							//Obtener el tiempo real que ha pasado desde que se inició la reproducción
							gettimeofday(&end, NULL);
							printf("Tiempo real desde que se reprodujo el primer paquete: %0.8f sec\n", time_diff(&start, &end));
							//Calcular el tiempo teórico de reproducción
							float tiempo_real = tarjetaSonido*requestedFragmentSize/(Bytes_per_sample*rate);
							printf("Tiempo teórico de reproducción: %lf sec\n", tiempo_real );
							printf("Número de paquetes enviados: %i\n", enviados);
							printf("Número de paquetes recibidos: %i\n", recibido);
							printf("Número de paquetes perdidos: %i\n", perdidos);
							printf("Número de paquetes de silencio insertados: %i\n", silencios);
							printf("Número de paquetes correctos insertados: %i\n", correctos);
						}
						exit(0);
				}
			}//else del selec
		}//si no hay datos bc
	}//while(1)
}//main