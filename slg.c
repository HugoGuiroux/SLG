/*
Copyright (C) 2013  
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>,
Fabien Mottet <fabien.mottet@inria.fr>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 or later, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <time.h>
#include <sys/un.h>
#define UNIX_PATH_MAX    108
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/time.h>

// Include the header
#include "slg.h"
#define DO_NOTHING 0
#define DO_WRITE 1
#define DO_READ 2

/** Main start and stop time */
struct timeval start_time;
struct timeval stop_time;

uint64_t start_time_cycles;

/** Global average stats of all clients */
long double global_avgCT;
unsigned long long nb_CT = 0;

long double global_avgRT = 0;
unsigned long long nb_RT = 0;

long double global_avgCRT = 0;
unsigned long long nb_CRT = 0;

/*global var are only available in compute stats*/

unsigned long global_errors;

unsigned long global_nbRequestsTaken;
unsigned long global_maxCT;
unsigned long global_minCT;

unsigned long long global_total_bytes_recv;
unsigned long long global_total_resp_recv;

unsigned long long file_nb_req;
unsigned long long file_nb_req_successful;
unsigned long long file_nb_reads;

/** Client data */
int currentClientPort;

/** For slave/daemon behaviour */
char * reportingAddress;
int slave_num;

/** Socket file descriptor */
int rs;
struct sockaddr_in sock_r, sock_w;
unsigned int len_w, len_r;

/** The two threads of the load generator */
pthread_t reading_thread;
pthread_t writing_thread;

/** application parameters with default values */
char * host;
unsigned int port;
unsigned int nb_clients;
unsigned int nb_msg_per_connection;
unsigned long long duration;
char is_unix;
unsigned int delay;

/** open-loop parameters **/
#define OPEN_CLIENT_COUNT_STARTUP 10
char open_active;
double open_mean;
double open_var;

/** All the clients will be save in a linked list starting by this pointer
    Also available in closed-loop as an array*/
client_t *all_clients;
unsigned int client_counter;

/** Options for sockets */
int tcp_no_delay;
int reuse_addr;
/** Linger on close if data present; socked will be closed immediatly
 * Linger is initialized during init_parameters
 */
struct linger linger;

/** buffers for socket inputs/outputs */
int sendwin;
int rcvwin;

/**Socket and var for accepting master connection**/
int waitingMasterPort;
int masterListenOrderSocket;
// My name
char name[256];
struct sockaddr_in server_addr;

//Master epoll for connect
int master_connect_epl;

//write thread fills it, read thread empties it.
circularBuffer * toReadBuffer;

//wreadrite thread fills it, write thread empties it.
circularBuffer * toWriteBuffer;

// FastCGI specific
static char* fastcgi_base_path;

#if UNIQUE_FILE_ACCESS_PATTERN
static char * file = "/index.html";

#elif SPECWEB99_FILE_ACCESS_PATTERN
/** Values used for specweb */
// Number of directories - based on load value
static int num_dirs;

// Zipf distribution table for directory
static double* dir_zipf;

#elif SPECWEB05_FILE_ACCESS_PATTERN
// Number of directories - based on SPECWEB05_SIMULTANEOUS_SESSIONS value
static int num_dirs;
#endif

static uint64_t proc_freq;

static int my_hostname;

// Generate random number using normal distribution N(mean, var)
// From: http://c-faq.com/lib/gaussian.html
#define PI 3.141592654
double gaussrand(double mean, double var)
{
	static double U, V;
	static int phase = 0;
	double Z;

	if(phase == 0) {
		U = (rand() + 1.) / (RAND_MAX + 2.);
		V = rand() / (RAND_MAX + 1.);
		Z = sqrt(-2 * log(U)) * sin(2 * PI * V);
	} else
		Z = sqrt(-2 * log(U)) * cos(2 * PI * V);

	phase = 1 - phase;

	return mean + var * Z;
}

uint64_t computeCPUhz(){
  uint64_t start,stop,acc;
  int i;
  int nbIter = 1;

  acc = 0;
  for(i = 0; i< nbIter; i++){
    rdtscll(start);
    usleep(1000000); //Sleep for approx 100ms
    rdtscll(stop);

    acc += stop - start;
  }
  acc = acc / nbIter;
#if DEBUG_LEVEL == 1
  DEBUG_TMP("Found %llu cycles for 100ms\n",(long long unsigned) acc);
#else
  DEBUG("Found %llu cycles for 100ms\n",(long long unsigned) acc);
#endif //DEBUG_LEVEL

  return acc*nbIter;
}


#if NON_BLOCKING_SOCKET
int setNonblocking(int fd) {

   DEBUG("Setting fd %d in a non-blocking mode\n",fd);
   int x;
   x = fcntl(fd, F_GETFL, 0);
   return fcntl(fd, F_SETFL, x | O_NONBLOCK);
}
#endif

void getSocketPeerInfo(int s, char * name, int len, unsigned short * p) {
   struct sockaddr_in csin;
   unsigned int size = sizeof(csin);
   int v = getpeername(s, (struct sockaddr*)&csin, &size);
   if (v!=0) {
      PANIC("warning, invalid fd: %d.\n", s);
   }
   strncpy(name, inet_ntoa(csin.sin_addr), len);
   *p = htons(csin.sin_port);
}

#if SPECWEB99_FILE_ACCESS_PATTERN
/**
 * Setup table of Zipf distribution values according to given size
 */
void setupZipf(double* table, int size) {

   double zipf_sum;
   int i;

   for (i = 1; i <= size; i++) {
      table[i-1] = (double)1.0 / (double)i;
   }

   zipf_sum = 0.0;
   for (i = 1; i <= size; i++) {
      zipf_sum += table[i-1];
      table[i-1] = zipf_sum;
   }

   table[size-1] = 0.0;
   table[0] = 0.0;
   for (i = 0; i < size; i++) {
      table[i] = 1.0 - (table[i] / zipf_sum);
   }
}

/**
 * Return index into Zipf table of random number chosen from 0.0 to 1.0
 */
int zipf(double* table) {
   double r = (double) rand() / ((double)RAND_MAX+1.);

   int i = 0;
   while (r < table[i]) {
      i++;
   }
   return i-1;
}

int get_new_value_from_table(double* table, int table_size) {
   double r = (double) rand() / ((double)RAND_MAX+1.);

   int i = 0;
   while (r > table[i] && r < table_size) {
      i++;
   }

   if(r==table_size){
      return -1;
   }
   else{
      return i;
   }
}


#elif SPECWEB05_FILE_ACCESS_PATTERN
/** See http://www.cse.usf.edu/~christen/tools/genzipf.c **/
int zipf(double alpha, int n)
{
   static int first = 1;          // Static first time flag
   static double c = 0;          // Normalization constant
   double z;                     // Uniform random number (0 < z < 1)
   double sum_prob;              // Sum of probabilities
   double zipf_value = 0;            // Computed exponential value to be returned
   int    i;                     // Loop counter

   // Compute normalization constant on first call only
   if (first)
   {
      for (i=1; i<=n; i++)
         c = c + (1.0 / pow((double) i, alpha));
      c = 1.0 / c;
      first = 0;
   }

   // Pull a uniform random number (0 < z < 1)
   do
   {
      z = (double) rand() / ((double)RAND_MAX+1.);
   }
   while ((z == 0) || (z == 1));

   // Map z to the value
   sum_prob = 0;
   for (i=1; i<=n; i++)
   {
      sum_prob = sum_prob + c / pow((double) i, alpha);
      if (sum_prob >= z)
      {
         zipf_value = i;
         break;
      }
   }

   // Assert that zipf_value is between 1 and N
   assert((zipf_value >=1) && (zipf_value <= n));

   return(zipf_value);
}

int get_new_value_from_table(double* table, int table_size) {
   double r = (double) rand() / ((double)RAND_MAX+1.);

   int i = 0;
   while (r > table[i] && r < table_size) {
      i++;
   }

   if(r==table_size){
      return -1;
   }
   else{
      return i;
   }
}

#endif

/**
 * Set default values for global variables
 */
void init_parameters() {
   /* Default values for client configuration */
   host = "localhost";
   port = 8080;
   is_unix = 0;
   nb_clients = 0;
   duration = 0;
   nb_msg_per_connection = 0;
   delay = 0;

   /* Open-loop default values */
   open_active = 0;
   open_mean = 0.f;
   open_var = 0.f;

   /* SOCKETS DEFAULT VALUES */
   // if 1 = naggle is disable, if 0 = naggle is enable
   tcp_no_delay = 1;

   // if 1 = bind can reuse local adresses
   reuse_addr = 1;

   // Initalizing linger
   linger.l_onoff = 1;
   /*0 = off (l_linger ignored), nonzero = on */
   linger.l_linger =0;
   /*0 = discard data, nonzero = wait for data sent */

   //buffers for socket inputs/outputs
   sendwin = OPT_USE_DEFAULT_SOCK_BUF_SIZE;
   rcvwin = OPT_USE_DEFAULT_SOCK_BUF_SIZE;

#if SPECWEB99_FILE_ACCESS_PATTERN
   /* Specweb initialization */
   
   num_dirs = (25 + (((400000.0 / 122000.0) * SPECWEB99_LOAD)/5.0));

   // class freq and file order have already been initialized
   dir_zipf = (double*) malloc(sizeof(double)*num_dirs);

   assert(dir_zipf!=NULL);

   setupZipf(dir_zipf,num_dirs);

   /* Initialisation du générateur aléatoire*/
   srand (time(NULL));

#elif SPECWEB05_FILE_ACCESS_PATTERN
   /* Specweb initialization */
   num_dirs = SPECWEB05_DIRSCALING * SPECWEB05_SIMULTANEOUS_SESSIONS;

   /* Initialisation du générateur aléatoire*/
   srand (0);

#elif SPECWEB09_BANKING_WORKLOAD
   // Sanity check that all lines probabilities are equals to one
   int i = 0;
   int j = 0;
   float sum = 0.f;

   for (i = 0; i < SPECWEB09_BANKING_WORKLOAD_PAGE_COUNT; i++) {
	   sum = 0.f;

	   for (j = 0; j < SPECWEB09_BANKING_WORKLOAD_PAGE_COUNT; j++)
		   sum += page_transitions[i][j];

	   if (fabs(sum - 1.f) > 0.00001)
		   PANIC("Sum of probabilities (%f) for page %d of SpecWeb09 Banking workload is not equal to 1\n", sum, i);
   }
#endif
}

/**
 * Init the socket of the distant server where load will go.
 */
void init_server_target(client_t* client, char *hostname, int port) {
   struct in_addr *ip_addr;

   // Unix socket
   if (port == 0) {
	   client->is_unix = 1;
	   memset(&client->soc_unx, 0, sizeof(struct sockaddr_un));
	   client->soc_unx.sun_family = AF_UNIX;
	   strncpy(client->soc_unx.sun_path, hostname, UNIX_PATH_MAX - 1);
	   client->soc_unx.sun_path[UNIX_PATH_MAX - 1] = '\0';
   } else {
	   client->is_unix = 0;
	   memset(&client->soc_tcp, 0, sizeof(struct sockaddr_in));

	   //Init remote server struct
	   if (hostname) {
		   client->ent = gethostbyname(hostname);
		   if (client->ent == NULL) {
			   PANIC("lookup on server's name \"%s\" failed\n", hostname);
		   }
		   ip_addr = (struct in_addr *)(*(client->ent->h_addr_list));
		   client->soc_tcp.sin_family = AF_INET;
		   bcopy(ip_addr, &(client->soc_tcp.sin_addr), sizeof(struct in_addr));
	   }

	   if (!client->ent) {
		   if (hostname){
			   PANIC("error - didn't get host info for %s\n", hostname);
		   }
		   else{
			   PANIC("error - never called gethostbyname\n");
		   }
	   }

	   if (port)
		   client->soc_tcp.sin_port = htons(port);
   }

}

/**
 * Change the stae of a client
 * This function protects client state manipulation
 */
void change_state(client_t *client, states_t new_state) {

   DEBUG("client %d change state %d to %d at %s\n", client->number, client->state, new_state, getCurrentTime());

   DEBUG("Changing state for client %d from ",client->number);
   switch (client->state) {
      case ST_READING:
         DEBUG("ST_READING to ");
         break;
      case ST_WRITING:
         DEBUG("ST_WRITING to ");
         break;
      case ST_CONNECT:
         DEBUG("ST_CONNECT to ");
         break;
      // Open open-loop never leave the ST_ENDED state
      case ST_ENDED:
      default: //ST_WAITING
         PANIC("%s() l.%d: Should not happen.\n", __FUNCTION__, __LINE__);
   }

   client->state = new_state;

   switch (new_state) {
      case ST_READING:
         DEBUG("ST_READING\n");
         break;
      case ST_WRITING:
         DEBUG("ST_WRITING\n");
         // Reinitializing values
         memset(client->read_hdr_buf,'\0',MAX_HDR_LENGTH);
         client->bytes_read = 0;
         client->bytes_write = 0;
         client->content_length = 0;
         client->header_length = 0;
         break;
      case ST_CONNECT:
         DEBUG("ST_CONNECT\n");
         // Reinitializing values
         client->nbRequestsOnIteration = 0;
         client->fd = -1;
         client->bytes_read = 0;
         client->bytes_write = 0;
         client->content_length = 0;
         client->header_length = 0;
         break;
      case ST_WAITING:
         DEBUG("ST_WAITING\n");
         //Nothing to do
         break;
      case ST_ENDED:
         DEBUG("ST_ENDED\n");
		 // The reader thread will close the connection
		 break;
   }

}

#define MULTIPLE_INTERFACES 0
#if MULTIPLE_INTERFACES

/*#define LOAD_BALANCING 1
static int current_target = 0;
#define NB_ITFS 20
static char* targets[NB_ITFS] = {
                           "192.168.20.100",
                           "192.168.21.100",
                           "192.168.22.100",
                           "192.168.23.100",
                           "192.168.24.100",
                           "192.168.25.100",
                           "192.168.26.100",
                           "192.168.27.100",
                           "192.168.28.100",
                           "192.168.29.100",
                           "192.168.30.100",
                           "192.168.31.100",
                           "192.168.32.100",
                           "192.168.33.100",
                           "192.168.34.100",
                           "192.168.35.100",
                           "192.168.36.100",
                           "192.168.37.100",
                           "192.168.38.100",
                           "192.168.39.100",
};*/


/*#define LOAD_BALANCING 0
static int current_target = 0;
#define NB_ITFS 6
static char* targets[NB_ITFS] = {
                           "192.168.20.100",
                           "192.168.21.100",
                           "192.168.22.100",
                           "192.168.23.100",
                           "192.168.24.100",
                           "192.168.25.100",
};*/


#define LOAD_BALANCING 0
static int current_target = 0;
#define NB_ITFS 20
static char* targets[NB_ITFS] = {
                           "192.168.20.100",
                           "192.168.21.100",
                           "192.168.22.100",
                           "192.168.23.100",
                           "192.168.24.100",
                           "192.168.25.100",
                           "192.168.26.100",
                           "192.168.27.100",
                           "192.168.28.100",
                           "192.168.29.100",
                           "192.168.30.100",
                           "192.168.31.100",
                           "192.168.32.100",
                           "192.168.33.100",
                           "192.168.34.100",
                           "192.168.35.100",
                           "192.168.36.100",
                           "192.168.37.100",
                           "192.168.38.100",
                           "192.168.39.100",
};


#if LOAD_BALANCING
static int interfaces_pending[NB_ITFS];
static int nbInit = 0;
#endif

#endif

void init_client_socket(client_t* client) {

#if MULTIPLE_INTERFACES
#if LOAD_BALANCING
   int min = -1;
   int itf = 0;
   for(itf = 0; itf < NB_ITFS; itf++){
      if(min < 0 || interfaces_pending[itf] < min) {
         current_target = itf;
         min = interfaces_pending[itf];
      }
   }

   nbInit ++;
   client->current_target = current_target;
   interfaces_pending[current_target]++;

   if(nbInit == 1000) {
      for(itf = 0; itf < NB_ITFS; itf++){
         printf("%s : %d\n", targets[itf], interfaces_pending[itf]);
      }
   }

#else
   current_target = (current_target + 1) % (sizeof(targets)/sizeof(char*));
#endif

   init_server_target(client, targets[current_target], port);
#endif


   //Getting Socket
   if (client->is_unix)
	   client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
   else
	   client->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);


   if (client->fd == -1) {
      perror("socket");
      exit(EXIT_FAILURE);
   }

   //Enables local address reuse
   if (setsockopt(client->fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr))
            < 0) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
   }

#ifdef USE_RST
   //Warning: this option make the client send a RST instead of a classic FIN on the tcp connection.
   //This sends a RST because this option make the socket especially port available directly after a close() call.
   //Do this only if we initiate the close

#if !CLOSE_AFTER_REQUEST
   if (setsockopt(client->fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger))
            < 0) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
   }
#endif //CLOSE_AFTER_REQUEST
#endif //USE_RST

#if NON_BLOCKING_SOCKET
   // Set the socket in a non-blocking mode
   setNonblocking(client->fd);
#endif



   //Take CT start time
   assert(gettimeofday(&client->CTstart,NULL) == 0);

   //Take CRT start time
   assert(gettimeofday(&client->CRTstart,NULL) == 0);
}

void add_client_to_master_write_set(client_t* client) {
    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
    ev.data.ptr = (void *)client;

    DEBUG("Adding client %d with fd %d to master write set\n", client->number, client->fd);
    assert(epoll_ctl(master_connect_epl, EPOLL_CTL_ADD, client->fd, &ev) == 0);
}

void remove_client_from_master_write_set(client_t* client) {
    assert(epoll_ctl(master_connect_epl, EPOLL_CTL_DEL, client->fd, NULL) == 0);
}

/**
 * Connect in non-blcking mode a client to the server
 */
states_t initialize_connect_client_to_server(client_t* client) {

   DEBUG("Call to %s\n",__FUNCTION__);
   if (client->fd == -1) {
      init_client_socket(client);
      DEBUG("client %d first time try to connect at %s\n",
               client->number,getCurrentTime());
   }

   int status = connect(client->fd, (struct sockaddr *)&(client->soc_address), client->is_unix ? sizeof(struct sockaddr_un) : sizeof(struct sockaddr_in));

   int err= errno;
   if ((status == -1) && (err != EINPROGRESS && err != EADDRNOTAVAIL && err != ECONNABORTED && err != EAGAIN)) {
      PRINT_ALERT("Client %d get an error for its connect. errno is %d (%s), status = %d\n",
               client->number, err, strerror(err), status);
      exit(EXIT_FAILURE);
   }
   else if (err == EADDRNOTAVAIL || err == EAGAIN) {
	  // In open loop, avoid flooding
	  if (!open_active) {
		  PRINT_ALERT("No more free local port. Will try later\n");
	  }

	  // On open-loop set the client to close to avoid spawning too much clients
      return open_active ? ST_ENDED : ST_CONNECT;
   }
   else if (err == ECONNABORTED) {
      //http://www.wlug.org.nz/ECONNABORTED
      PRINT_ALERT("Client %d get an error for its connect. errno is ECONNABORTED (nb_clients: %d)\n",
               client->number, nb_clients);

	  // On open-loop set the client to close to avoid spawning too much clients
      return open_active ? ST_ENDED : ST_CONNECT;
      //exit(EXIT_FAILURE);
   }

   if(status == 0){
      if (client->is_unix == 0)
	     assert(0 && "Connect return 0 immediately ? (Should not happen but if so, we must add a finalize_client_connect.)");

      // Connect OK
      add_client_to_master_write_set(client);

      return ST_WRITING;

   }
   else{
      DEBUG("Initiate non blocking connect for client %d\n",client->number);
      add_client_to_master_write_set(client);
      client->connect_in_progress = 1;

      return ST_CONNECT;
   }
}


/* The signal SIGPIPE handler function */
void sigpipe_handler(int signal) {
   /** IGNORE **/
   PRINT_ALERT("ignoring SIGPIPE: %d\n", signal);
}

void sigterm_handler(int signal) {
  printf("SIGTERM received. Exiting.\n");
  exit(0*signal);
}


/**
 * Finalize the connection of the client when a non-blocking connect completion
 * was wait on select.
 */
states_t finalize_connect_client_to_server(client_t* client) {
   DEBUG("Call to %s\n",__FUNCTION__);

   //Handle connect select.
   client->connect_in_progress = 0;

   // Socket selected for write
   socklen_t lon = sizeof(int);
   int valopt;
   if (getsockopt(client->fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon) < 0) {
      PRINT_ALERT("Error for client %d in getsockopt() %d - %s\n", client->number, errno, strerror(errno))      ;
      exit(0);
   }
   // Check the value returned...
   if (valopt) {

      switch (valopt) {
         case ETIMEDOUT:
            DEBUG("client %d connect ETIMEDOUT at %s\n", client->number, getCurrentTime());
			// On open-loop set the client to close to avoid spawning too much clients
			return open_active ? ST_ENDED : ST_CONNECT;
         case ECONNREFUSED:
               client->nb_connect_attempts ++;
               if (client->nb_connect_attempts >= NB_CONNECT_ATTEMPTS_MAX) {
                  PANIC("To much ECONNREFUSED (%d). exiting.\n", NB_CONNECT_ATTEMPTS_MAX);
               }
               PRINT_ALERT("client %d ECONNREFUSED nb_connect_attempts: %d at %s\n", client->number,
               		client->nb_connect_attempts, getCurrentTime());
			   // On open-loop set the client to close to avoid spawning too much clients
			   return open_active ? ST_ENDED : ST_CONNECT;

               /** Critical error cases **/
         case EINPROGRESS:
         case EALREADY:
         default:
            perror("connect");
            PANIC("Client %d, error while trying to connect. Errno is %d (%s)\n",client->number,errno, strerror(valopt));
      }
   }

   client->nb_connect_attempts = 0;

   /* End of connect */
   struct timeval stop_time;
   assert(gettimeofday(&stop_time,NULL) == 0);


   long double ct_time = (long double)compare_time(&(client->CTstart), &stop_time);
   SET_IF_MIN(global_minCT, ct_time);
   SET_IF_MAX(global_maxCT, ct_time);
   global_avgCT += ct_time;
   nb_CT++;

   DEBUG("client %d CT: %lu at %s\n",
            client->number,
            compare_time(&(client->CTstart), &stop_time),
            getCurrentTime());

   DEBUG("Client %d is now connected\n",client->number);

   struct sockaddr_in csin;
   unsigned int size = sizeof(csin);
   getsockname(client->fd, (struct sockaddr*)&csin, &size);

   // A client will always use the same port number
   client->port = ntohs(csin.sin_port);

   /* Updating socket values */
   // Disable Naggle algorithm ?
   if (setsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, (char *)&tcp_no_delay, sizeof(tcp_no_delay))
            < 0) {
      PRINT_ALERT("Error with fd %d\n", client->fd);
      perror("setsockopt TCP_NODELAY");
   }

   //Set send buffer size
   if (setsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, &sendwin, sizeof(sendwin))
            < 0) {
      PRINT_ALERT("Error with fd %d\n", client->fd);
      perror("setsockopt SO_SNDBUF");
   }

   //Set receive buffer size
   if (setsockopt(client->fd, SOL_SOCKET, SO_RCVBUF, &rcvwin, sizeof(rcvwin))
            < 0) {
      PRINT_ALERT("Error with fd %d\n", client->fd);
      perror("setsockopt SO_RCVBUF");
   }

   // Updating client info
   change_state(client, ST_WRITING);

   return ST_WRITING;
}

/**
 * Choose an url (default or specweb-like distribution
 */
char * choose_url(__attribute__((unused)) client_t* client) {

   char * url;

#if SPECWEB99_FILE_ACCESS_PATTERN

   /* Use a specweb distribution */
   int dir = zipf(dir_zipf);

   int theclass = get_new_value_from_table((double*) class_freq, SPECWEB99_NB_CLASS);
   int file = get_new_value_from_table((double *) file_freq, SPECWEB99_NB_FILES);

   assert(theclass>=0 && theclass < SPECWEB99_NB_CLASS);
   assert(file>=0 && file < SPECWEB99_NB_FILES);

   // URL size must be less than 63 (+\0)
   url = calloc(64,sizeof(char));
   assert(url != NULL);

   snprintf(url,63,"%sdir%05d/class%d_%d",DEFAULT_DIR,dir,theclass,file);

#elif SPECWEB05_FILE_ACCESS_PATTERN
   double d = (double) rand () / ((double)RAND_MAX+1.);

   int url_size = 128;

   if(d < type_freq[0]){
      url = calloc(url_size,sizeof(char));
      assert(url != NULL);

      int d2 = rand () % IMAGES_NB_FILES;
      snprintf(url,url_size,"/support/images/%s",images_files[d2]);
   }
   else if(d < type_freq[1]){
      url = calloc(url_size,sizeof(char));
      assert(url != NULL);

      int d2 = rand () % PHP_NB_FILES;
      snprintf(url,url_size,"/support/%s",php_files[d2]);
   }
   else {
      /* Use a specweb distribution */
      int dir = zipf(SPECWEB05_ZIPF_ALPHA, num_dirs);

      // TODO Check it
      int class = get_new_value_from_table((double*) class_freq, SPECWEB05_NB_CLASS);
      int file = get_new_value_from_table((double *) file_freq[class], SPECWEB05_MAX_FILE_PER_CLASS);

      if(class == -1 || file == -1 || file_freq[class][file] == -1){
         PANIC("Big bug in SpecWeb05\n");
      }

      // URL size must be less than 63 (+\0)
      url = calloc(url_size,sizeof(char));
      assert(url != NULL);

      snprintf(url,url_size,"/support/downloads/dir%010d/download%d_%d",dir,class,file);
   }

#elif SPECWEB09_BANKING_WORKLOAD
   double d = (double) rand () / ((double)RAND_MAX+1.);
   float sum = open_active ? vector_transitions[0] : page_transitions[client->current_url][0];
   int url_size = 128;

   int i = 1;

   while (i < SPECWEB09_BANKING_WORKLOAD_PAGE_COUNT && sum <= d) {
       // In open-loop, use steady state vector
       if (open_active) {
           sum += vector_transitions[i++];
       }
       // In close-loop, use markov transition table
       else {
           sum += page_transitions[client->current_url][i++];
       }
   }

   if (i == SPECWEB09_BANKING_WORKLOAD_PAGE_COUNT && sum <= d)
       PANIC("Seeking URL for SpecWeb09 banking and goes out. Check probabilities -- or algorithm\n");

   url = calloc(url_size, sizeof(char));
   strncpy(url, page_mapping[i - 1], url_size - 1);
   url[url_size - 1] = '\0';
   client->current_url = i - 1;

#elif UNIQUE_FILE_ACCESS_PATTERN
   /* Use always the same url */
   url = file;
#endif

   DEBUG("[Client %d] URL : %s\n", client->number, url);
   return url;
}

/**
 * Build an http request
 */
void build_http_request(client_t* client) {

   char* url = choose_url(client);
   client->req_unique_id++;

   int req_length = snprintf(client->request_buf, REQUEST_BUFFER_SIZE,
            "GET %s HTTP/1.1\r\n"
            //"Request unique id: %d-%u-%d\r\n"
            "Host: %s\r\n"
            "Accept: text/plain,text/html,*/*\r\n\r\n",
            url,
            //client->number, client->req_unique_id,my_hostname,
            client->is_unix ? "localhost" : host);

#if !UNIQUE_FILE_ACCESS_PATTERN
   free(url);
#endif

   DEBUG("Client %d will send : %s", client->number, client->request_buf);


   /* We accept all type of response */
   client->request_total_len = req_length;
   assert(client->request_total_len == strlen(client->request_buf));

   assert(client->request_total_len < REQUEST_BUFFER_SIZE);
}

#if FASTCGI_PROTOCOL

/**
 * Build a FastCGI request
 * See http://www.fastcgi.com/devkit/doc/fcgi-spec.html#SB
 * Note: suppose big endian
 **/
void build_fastcgi_request(client_t* client) {
   char* url = choose_url(client);
   client->req_unique_id++;
   size_t size = 0;
   unsigned int i = 0;

   FCGI_Header header;
   header.version = FCGI_VERSION_1;
   header.requestId = htons(1);
   header.reserved = 0;
   header.paddingLength = 0; // May be used to byte-align with content

   // {FCGI_BEGIN_REQUEST,   1, {FCGI_RESPONDER, 0}}
   FCGI_BeginRequestBody begin_request_body;
   header.type = FCGI_BEGIN_REQUEST;
   begin_request_body.role = htons(FCGI_RESPONDER);
   begin_request_body.flags = 0;
   header.contentLength = htons(sizeof(begin_request_body));

   FCGI_BeginRequestRecord begin_request_record;
   begin_request_record.header = header;
   begin_request_record.body = begin_request_body;
   memcpy(client->request_buf, (char *)&begin_request_record, MIN(REQUEST_BUFFER_SIZE - size, sizeof(begin_request_record)));
   size += sizeof(begin_request_record);
   assert(size < REQUEST_BUFFER_SIZE);

   // {FCGI_PARAMS,          1, "\013\002SERVER_PORT80\013\016SERVER_ADDR199.170.183.42 ... "}
   header.type = FCGI_PARAMS;

   // Keep content length offset to update latter
   char *content_length_offset = client->request_buf + size + offsetof(FCGI_Header, contentLength);

   // Write the FCGI_PARAMS header
   memcpy(client->request_buf + size, (char *)&header, MIN(REQUEST_BUFFER_SIZE - size, sizeof(header)));
   size += sizeof(header);
   assert(size < REQUEST_BUFFER_SIZE);

   // "Lengths of 127 bytes and less can be encoded in one byte" << TODO
   size_t before_size = size;
   size_t url_length = strlen(url);

   char *token = strchr(url, '?');
   char *fastcgi_absolute_url = NULL;
   char *script_name = NULL;
   char *query_string = NULL;
   size_t base_path_length = strlen(fastcgi_base_path);

   if (token == NULL) {
	   fastcgi_absolute_url = calloc(base_path_length + url_length + 1, sizeof(char));
	   assert(fastcgi_absolute_url != NULL);
	   strcpy(fastcgi_absolute_url, fastcgi_base_path);
	   strcat(fastcgi_absolute_url, url);
	   script_name = url;
	   query_string = NULL;
   } else {
	   fastcgi_absolute_url = calloc(base_path_length + (token - url) + 1, sizeof(char));
	   strcpy(fastcgi_absolute_url, fastcgi_base_path);
	   strncat(fastcgi_absolute_url, url, token - url);
       fastcgi_absolute_url[base_path_length + (token - url)] = '\0';

	   script_name = calloc(token - url + 1, sizeof(char));
	   strncpy(script_name, url, token - url);
       script_name[token - url] = '\0';

	   query_string = calloc(url_length - (token - url + 1) + 1, sizeof(char));
	   strcat(query_string, (char *)(token + 1));
   }

   const char *params[] = {
	   "SCRIPT_FILENAME", fastcgi_absolute_url,
	   "QUERY_STRING", query_string,
	   "REQUEST_METHOD", "GET",
	   "CONTENT_TYPE", "",
	   "CONTENT_LENGTH", "",
	   "SCRIPT_NAME", script_name,
	   "REQUEST_URI", url,
	   "DOCUMENT_ROOT", fastcgi_base_path,
	   "SERVER_PROTOCOL", "HTTP/1.1",
	   "GATEWAY_INTERFACE", "CGI/1.1",
	   "SERVER_SOFTWARE", "slg",
	   "REMOTE_ADDR", "127.0.0.1",
	   "REMOTE_PORT", "12345",
	   "SERVER_ADDR", client->is_unix ? "127.0.0.1" : client->ent->h_addr,
	   "SERVER_PORT", "80",
	   "SERVER_NAME", "localhost",
	   "REDIRECT_STATUS", "200",
	   "HTTP_HOST", client->is_unix ? "localhost" : client->ent->h_addr,
	   "HTTP_USER_AGENT", "slg",
	   "HTTP_ACCEPT", "text/plain,text/html,*/*"
   };

   for (i = 0; i < sizeof(params)/sizeof(char*); i += 2) {
	   /**
		  typedef struct {
		  uint32_t nameLength;
		  uint32_t valueLength;
		  uint8_t name[nameLength];
		  uint8_t value[valueLength];
		  } FCGI_NameValuePair;
	   */

	   const char* name = params[i];
	   const char* value = params[i + 1];

	   uint32_t nameLength = name ? strlen(name) : 0;
	   uint32_t valueLength = value ? strlen(value) : 0;

	   /**
		* The high-order bit of the first byte of a length indicates the
		* length's encoding. A high-order zero implies a one-byte encoding, a
		* one a four-byte encoding. */
	   uint32_t nameLengthBigEndian = htonl(nameLength  | (uint32_t)(1 << 31));
	   uint32_t valueLengthBigEndian = htonl(valueLength  | (uint32_t)(1 << 31));

	   memcpy(client->request_buf + size, &nameLengthBigEndian, MIN(REQUEST_BUFFER_SIZE - size, sizeof(nameLength)));
	   size += sizeof(nameLength);

	   memcpy(client->request_buf + size, &valueLengthBigEndian, MIN(REQUEST_BUFFER_SIZE - size, sizeof(valueLength)));
	   size += sizeof(valueLength);

	   if (nameLength > 0) {
		   memcpy(client->request_buf + size, name, MIN(REQUEST_BUFFER_SIZE - size, nameLength));
		   size += nameLength;
	   }

	   if (valueLength > 0) {
		   memcpy(client->request_buf + size, value, MIN(REQUEST_BUFFER_SIZE - size, valueLength));
		   size += valueLength;
	   }

	   assert(size < REQUEST_BUFFER_SIZE);
   }

   // Update the contentLength size
   *(uint16_t*)(content_length_offset) = htons(size - before_size);

   // {FCGI_PARAMS,          1, ""}
   header.contentLength = 0;
   memcpy(client->request_buf + size, &header, MIN(REQUEST_BUFFER_SIZE - size, sizeof(header)));
   size += sizeof(header);
   assert(size < REQUEST_BUFFER_SIZE);

   // {FCGI_STDIN,           1, ""}
   header.type = FCGI_STDIN;
   memcpy(client->request_buf + size, &header, MIN(REQUEST_BUFFER_SIZE - size, sizeof(header)));
   size += sizeof(header);
   assert(size < REQUEST_BUFFER_SIZE);

   free(fastcgi_absolute_url);
   if (token != NULL)
	   free(script_name);
   if (query_string != NULL)
	   free(query_string);
#if !UNIQUE_FILE_ACCESS_PATTERN
   free(url);
#endif

   DEBUG("Client %d will send : %s", client->number, client->request_buf);


   /* We accept all type of response */
   client->request_total_len = size;
   assert(client->request_total_len < REQUEST_BUFFER_SIZE);
}
#endif

#define MIX_GET_SET     10 // X% of set ops
#define VALUE_SIZE      450 // Be careful with REQUEST_BUFFER_SIZE
#define HOW_MANY_KEYS   10000

void build_memcached_request(client_t* client) {
   static int next_key = 0;
   int req_length;
   if(rand()%100 >= MIX_GET_SET) {
      static char * get_requests[HOW_MANY_KEYS];
      static int get_req_length[HOW_MANY_KEYS];

      static int already_memset = 0;
      if(!already_memset){
         int i = 0;
         for(i = 0; i < HOW_MANY_KEYS; i++){
            int ret = asprintf(
                     &get_requests[i],
                     "get key%d\r\n",
                     i
                     );

            if(ret == -1){
               PRINT_ALERT("Error\n");
               exit(-1);
            }
            get_req_length[i] = ret;
         }
         already_memset = 1;
      }

      req_length = get_req_length[next_key];
      memcpy(client->request_buf, get_requests[next_key],req_length);
   } else {
      static char * set_requests[HOW_MANY_KEYS];
      static int set_req_length[HOW_MANY_KEYS];

      static int already_memset = 0;
      if(!already_memset){
         int i = 0;
         for(i = 0; i < HOW_MANY_KEYS; i++){
            int ret = asprintf(
                     &set_requests[i],
                     "set key%d 0 0 %d\r\n%*.*s\r\n",
                     i,
                     VALUE_SIZE, VALUE_SIZE, VALUE_SIZE,
                     "value");

            if(ret == -1){
               PRINT_ALERT("Error\n");
               exit(-1);
            }
            set_req_length[i] = ret;
         }
         already_memset = 1;
      }

      req_length = set_req_length[next_key];
      memcpy(client->request_buf, set_requests[next_key],req_length);
   }

   client->request_total_len = req_length;
   next_key = (next_key+1)%HOW_MANY_KEYS;

   DEBUG("Client %d will send : %s", client->number, client->request_buf);
}


/*
 * note on is_in_last_loop_and_not_last_request param:
 *	if 0: first call
 *	if -1 recurssive call somewhere else
 *	if  1 recurssive call in last loop and not last request
 */
states_t treat_error(client_t* client) {

   client->errors++;

   DEBUG_TMP("Client %d on port %d get %lu error reading\n",client->number, client->port, client->errors);

   // Free the outgoing port
   int status = close(client->fd);
   if(status==-1){
      PANIC("Client %d on port %d get an error closing socket in treat error\n",client->number, client->port);
   }

   client->fd = -1;

   struct timeval read_complete_time;
   assert(gettimeofday(&read_complete_time,NULL) == 0);

   //CRT have to be taken here for errors.
   global_avgCRT += (long double) compare_time(&(client->CRTstart), &read_complete_time);
   nb_CRT += client->nbRequestsOnIteration;

   if (open_active) {
	   // Mark this connection as need to be closed (ST_ENDED). The connection will be closed by the reader thread
	   change_state(client, ST_ENDED);
   }
   else {
	   //this connection is ended, let's close it and open another to send burst of requests
	   //DEBUG("Client %d ended its %dth loop. state:%d\n", client->number, client->nbIterations, client->state);
	   change_state(client, ST_CONNECT);
   }

   return client->state;
}

/**
 * Send a request to the server
 */
states_t send_request(client_t *client) {

   //Check FD health
   socklen_t lon = sizeof(int);
   int valopt;
   if (getsockopt(client->fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon) < 0) {
      PANIC("Error for client %d in getsockopt() %d - %s\n",
               client->number, errno, strerror(errno));
   }
   // Check the value returned...
   if (valopt) {
      unsigned short port;
      char temp[128];
      bzero(temp, 128);
      switch (valopt) {
         case EPIPE:
            getSocketPeerInfo(client->fd, temp, 128, &port);
            PRINT_ALERT("client %d out of w_select client_port:%hu\n", client->number, client->port);
         /** Critical error cases **/
         default:
            perror("connect");
            PANIC("Client %d, error after a select for a write. Errno is %d (%s)\n",client->number,errno, strerror(valopt));
      }
   }

   int res;

   if (client->bytes_write == 0) {
      //||||-Probe RT||||//
      assert(gettimeofday(&client->RTstart,NULL) == 0);

   }

   DUMP_REQUEST(client);
   res = write(client->fd, &client->request_buf[client->bytes_write],
            (client->request_total_len - client->bytes_write));

   file_nb_req++;

   if (res> 0) {
      client->bytes_write += res;
   }
   else if (res == 0 || (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) { //res <= 0
      PRINT_ALERT("write not complete ?!\n");
      // Socket is in a non-blocking mode
      // Continue is needed
      return client->state;
   }
   else if (res == -1 && errno == EPIPE) {
      int temp_errno = errno;
      unsigned short port;
      char temp[128];
      bzero(temp, 128);
      PRINT_ALERT("EPIPE on socket %d\n", client->fd);
      getSocketPeerInfo(client->fd, temp, 128, &port);
      PANIC("fd %d errno:%d -> %s:%d client_port:%hu\n", client->fd, temp_errno, temp, port, client->port);
      PANIC("Client %d on port %d get an error writing on socket %d.\n",client->number, client->port, client->fd);
   }
   else {
      // There is a write error
      perror("***** Write error ******");
      PANIC("Write error: %d (EAGAIN is %d), res is %d\n",errno,EAGAIN,res);
   }

   if (client->bytes_write == client->request_total_len) {
      DEBUG("Client %d (port %d) sent a new request\n", client->number, client->port);
      change_state(client, ST_READING);
   }

   return client->state;
}

/**
 * Function called when all the data has been read
 */
void read_complete(client_t* client) {
   DEBUG("Client %d (port %d) succefully read a response for request %d\n",
            client->number, client->port, client->nbRequestsOnIteration+1);

   /* Calculate the request time */
   struct timeval read_complete_time;
   assert(gettimeofday(&read_complete_time,NULL) == 0);

   global_avgRT += (long double) compare_time(&(client->RTstart), &read_complete_time);
   nb_RT ++;

   file_nb_req_successful++;

   // Add bytes received
   /*client->total_bytes_recv += client->header_length;
   client->total_bytes_recv += client->bytes_read;*/
   client->total_resp_recv++;

   client->nbRequestsOnIteration++;

   // We will process another request with this connection
   if (client->nbRequestsOnIteration < nb_msg_per_connection) {
      //need to process another request on this connection
      change_state(client, ST_WRITING);
   }

   // This is the end of the loop. Checking if we need to open a new connection or to end
   else {
      if (close(client->fd) == -1) {
         PRINT_ALERT("Error when closing socket %d (errno %d : %s)\n",client->fd,errno, strerror(errno));
         perror("Close: ");
         _exit(-1);
      }

      global_avgCRT += (long double) compare_time(&(client->CRTstart), &read_complete_time);
      nb_CRT += client->nbRequestsOnIteration;

      client->fd = -1;


#if MULTIPLE_INTERFACES && LOAD_BALANCING
      interfaces_pending[client->current_target]--;
#endif

	  DEBUG("Client %d ended its iteration\n", client->number);

	  if (open_active) {
		  // Mark connection as ST_ENDED, will be closed after
		  change_state(client, ST_ENDED);
	  }
	  else {
		  //this connection is ended, let's close it and open another to send burst of requests
		  change_state(client, ST_CONNECT);
	  }

   }//end else if

#if !IGNORE_HTTP_CONTENT
   free(client->read_content_buf);
#endif
}


static void read_headers(client_t *client){
   int status;
   DEBUG("read_headers called\n");

   status = read(client->fd, client->read_hdr_buf + client->bytes_read, MAX_HDR_LENGTH - client->bytes_read);
   client->total_bytes_recv += status;

   file_nb_reads++;

   if (status < 0) {
      switch (errno) {
         case EAGAIN:
            status = 0;
            return;
         case ECONNRESET:
#if !CLOSE_AFTER_REQUEST
            PANIC("Client %d on port %d get an ECONNRESET reading on socket %d. end.\n",client->number, client->port, client->fd);
#else
            read_complete(client);
#endif //CLOSE_AFTER_REQUEST
            return;
         case EPIPE:
            PANIC("Received a SIGPIPE on socket %d\n",client->fd);
         default:
            perror("Read Error");
            PANIC("Unwanted read error on socket %d for %d\n", client->fd, client->number);
      }
   }
   else if (status == 0) {
      //EOF
      treat_error(client);
      return;
   }

#if !CLOSE_AFTER_REQUEST
   client->bytes_read += status;

   //put to 0 to avoid strstr to find dumb things
   client->read_hdr_buf[client->bytes_read] = '\0';

   // Search for headers
#ifdef MEMCACHED_PROTOCOL
   char * headers_end = strstr(client->read_hdr_buf, "\r\n");
   if(headers_end != NULL) {
      if(strncmp("VALUE", client->read_hdr_buf, sizeof("VALUE")-1)) {
         if(!strncmp("END", client->read_hdr_buf, sizeof("END")-1)) {
            DEBUG("Client %d has read end\n", client->number);
            read_complete(client);
            return;
         }
         if(!strncmp("STORED", client->read_hdr_buf, sizeof("STORED")-1)) {
            DEBUG("Client %d has read STORED\n", client->number);
            read_complete(client);
            return;
         }
         PANIC("Unexpected header %s\n", client->read_hdr_buf);
      }

      char key[512];
      int flag;
      sscanf(client->read_hdr_buf, "VALUE %s %d %d\r\n", key, &flag, &client->content_length);
      DEBUG("Client %d key %s Length %d\n", client->number, key, client->content_length);
      client->content_length += sizeof("\r\nEND\r\n") - 1;

#if !IGNORE_HTTP_CONTENT
      client->read_content_buf = (char*) malloc (client->content_length * sizeof(char));
      if(client->header_length < client->bytes_read){
         memcpy(client->read_content_buf, client->request_buf + client->header_length, client->bytes_read-client->header_length);
      }
#endif
      client->bytes_read -= client->header_length;

      char * end = strstr(client->read_hdr_buf, "END\r\n");
      if(end) {
         DEBUG("Client %d has read end (client->read_hdr_buf=%s)", client->number,client->read_hdr_buf);
         read_complete(client);
         return;
      }
   } else if (client->bytes_read >= MAX_HDR_LENGTH){
      PANIC("Cannot find headers. MAX_HDR_LENGTH is probably to small\n");
   }
#else

   size_t padding = 0;

#if FASTCGI_PROTOCOL
   padding = sizeof(FCGI_Header);

   if (strncmp(client->read_hdr_buf + padding, "Primary script unknown", 22) == 0) {
	   PRINT_ALERT("****** WARNING - Get an unwanted header response ******\n");
	   PRINT_ALERT("Header: %s\n",   client->read_hdr_buf + padding);
	   treat_error(client);
	   return;
   }
#endif
   char * headers_end = strstr(client->read_hdr_buf + padding, "\r\n\r\n");

   if (headers_end != NULL) {
      // Firsty check if the response is not 404
      /*
      if (strcasestr(client->read_hdr_buf, "404 Not Found") != NULL) {
         ;
      }
      */
      //if (strcasestr(client->read_hdr_buf, "200 OK") == NULL && strcasestr(client->read_hdr_buf, "404 Not Found") == NULL ) {
#if !FASTCGI_PROTOCOL
	   if (strcasestr(client->read_hdr_buf, "200 OK") == NULL) {
         PRINT_ALERT("****** WARNING - Get an unwanted header response ******\n");
         PRINT_ALERT("Header: %s\n",   client->read_hdr_buf + padding);
         treat_error(client);
         return;
      }
#endif

      client->header_length = headers_end - client->read_hdr_buf + 4 + padding;
      DEBUG("Header length : %d\n",client->header_length);
      DEBUG("Hdr : \n%s\n", client->read_hdr_buf + padding);

      // Now parse the headers
      char * cl = strstr(client->read_hdr_buf + padding, "Content-Length:");

      if(!cl){
         PRINT_ALERT("%s", client->read_hdr_buf);
         PANIC("Warning content length must be specified in server response\n");
      }

	  client->content_length = atoi(cl+15);

      DEBUG("Content length : %u\n", client->content_length);

      /** Allocating buffer for file **/
#if IGNORE_HTTP_CONTENT
      client->read_content_buf = NULL;
#else
      client->read_content_buf = (char*) malloc (client->content_length * sizeof(char));
      assert(client->read_content_buf != NULL);
      if(client->header_length < client->bytes_read){
         memcpy(client->read_content_buf, client->request_buf + client->header_length, client->bytes_read-client->header_length);
      }
#endif


#if DEBUG_RUID
      char* unique_id = strstr(client->read_buf, "Request unique id");
      if(unique_id){
         int client_num = atoi(unique_id + 19*sizeof(char));
         char* sep = strstr(unique_id + 19*sizeof(char), "-");
         int client_req_id = atoi(sep+1);
         char* sep2 = strstr(sep+1, "-");
         int client_hostname = atoi(sep2+1);

         if(client_num != client->number
                  || client_req_id != (int) client->req_unique_id
                  || client_hostname != my_hostname){

            PANIC("Unexpected_response !!!!!!\n"
                     "Wanted uid: %d-%d-%d\n"
                     "Received uid: %d-%d-%d\n",
                     client->number, (int) client->req_unique_id, my_hostname,
                     client_num, client_req_id, client_hostname);
         }
      }
      else{
         PANIC("Response must have a unique id !\n");
      }
#endif // DEBUG_RUID

      client->bytes_read -= client->header_length;
      if (client->bytes_read == client->content_length) {
	      // Ok the request has been answered correctly
	      read_complete(client);
      }
   }
   else if (client->bytes_read >= MAX_HDR_LENGTH){
      PANIC("Cannot find headers. MAX_HDR_LENGTH is probably to small\n");
   }

#endif
#else
   assert(0); //shouldn't have something to read
#endif //CLOSE_AFTER_REQUEST
}

static inline void read_content(client_t *client){
   int status;
   DEBUG("Read content called for fd %d, content length = %d, remaining %d bytes\n",
            client->fd,
            client->content_length,
            client->content_length-client->bytes_read);

#if !IGNORE_HTTP_CONTENT
   status = read(client->fd, client->read_content_buf + client->bytes_read, client->content_length - client->bytes_read);
#else
   char buffer[MAX_HDR_LENGTH];
   status = read(client->fd, buffer, sizeof(buffer));
#endif
   client->total_bytes_recv += status;

   file_nb_reads++;

   if (status < 0) {
      switch (errno) {
         case EAGAIN:
            status = 0;
            return;
         case ECONNRESET:
            PANIC("Client %d on port %d get an ECONNRESET reading on socket %d. end.\n",client->number, client->port, client->fd);
            return;
         case EPIPE:
            PANIC("Received a SIGPIPE on socket %d\n",client->fd);
         default:
            perror("Read Error");
            PANIC("Unwanted read error on socket %d\n", client->fd);
      }
   }
   else if (status == 0) {
#if !IGNORE_HTTP_CONTENT
      //EOF
      PANIC("Get EOF\n");

      treat_error(client);
#else
	  read_complete(client);
#endif
      return;
   }

#if !CLOSE_AFTER_REQUEST
   client->bytes_read += status;
   DEBUG("Bytes read %d/%d\n",client->bytes_read,client->content_length);

   // If we have read headers and all data then entire response has been read
   if (client->bytes_read == client->content_length) {
      // Ok the request has been answered correctly
      read_complete(client);
   }
#else
   assert(0); //shouldn't have something to read
#endif //CLOSE_AFTER_REQUEST
}

/**
 * Read a server response
 */
states_t read_response(client_t *client) {
   DEBUG("read_response called\n");

   if(client->header_length == 0){
      read_headers(client);
   }
   else{
      read_content(client);
   }

   return client->state;
}


/**
 * Init the clients struct
 */
void init_clients() {

   unsigned int i;
   unsigned int total_clients = open_active ? OPEN_CLIENT_COUNT_STARTUP : nb_clients;
   client_t * client;
   client_t * open_client_list[OPEN_CLIENT_COUNT_STARTUP];

   if (!open_active) {
	   all_clients = (client_t*) calloc(total_clients, sizeof(client_t));
	   assert(all_clients!=NULL);
   }
   else {
	   for (i = 0; i < OPEN_CLIENT_COUNT_STARTUP; i++) {
		   open_client_list[i] = (client_t*) calloc(1, sizeof(client_t));
		   assert(open_client_list[i] != NULL);
	   }
	   all_clients = open_client_list[0];
   }

   client_counter = 0;

   for (i=0; i < total_clients; i++) {

	   if (open_active) {
		   client_counter++;
		   client = open_client_list[i];
	   }
	   else {
		   client = &all_clients[i];
	   }

	   client->number = i;

	   //no valid fd at the beginning
	   client->fd = -1;

	   //initial client state
	   DEBUG("client %d initialized at %s\n", i, getCurrentTime());
	   client->state = ST_CONNECT;

	   //after init_client, the client will do a new request
	   client->new_request = 1;

#if !MULTIPLE_INTERFACES
	   init_server_target(client, host, port);
#endif
	   if (i + 1 == total_clients) {
		   client->next = NULL;
	   }
	   else {
		   if (open_active) {
			   client->next = open_client_list[i + 1];
		   }
		   else {
			   client->next = &all_clients[i + 1];
		   }
	   }
	   if (i == 0) {
		   client->prev = NULL;
	   }
	   else {
		   if (open_active) {
			   client->prev = open_client_list[i - 1];
		   }
		   else {
			   client->prev = &all_clients[i - 1];
		   }
	   }
   }

   //init stats
   global_avgCT = 0;
   nb_CT = 0;

   global_avgRT = 0;
   nb_RT = 0;

   global_avgCRT = 0;
   nb_CRT = 0;
}

/**
 * Check if the parameters are good
 */
void check_parameters() {
   if (nb_clients>MAX_CLIENTS) {
      PANIC("The number of clients must be between %d and %d\n", 1 , MAX_CLIENTS);
   }
   if (port == 0 && is_unix == 0) {
      PANIC("Distant server port must be between %d and %d\n", 1 , 65535);
   }
   if (duration == 0) {
      PANIC("Test duration must be greater/equal than/to %d\n", 1);
   }
   if (nb_msg_per_connection == 0) {
      PANIC("nb_msg_per_connection must be greater/equal than/to %d\n", 1);
   }
#ifdef FASTCGI_PROTOCOL
   if (fastcgi_base_path == NULL) {
	   PANIC("fastcgi_base_path missing but SLG compiled with FASTCGI_PROTOCOL\n");
   }
#endif
   if (open_active && (open_mean <= 0.f || open_var <= 0.f)) {
	   PANIC("Configured in open-loop but open_mean or open_var is not strictly greater than 0.0\n");
   }
}

/**
 * Compute the stats gathered during the load injection.
 * Value are computed only on 80% of the sample.
 */
void compute_stats() {
   client_t *client;

   //init globale stats:
   global_nbRequestsTaken = 0;
   global_nbRequestsTaken = 0;

   global_errors = 0;

   //init the global min and max CT.
   global_maxCT = 0;
   global_minCT = (unsigned long) -1;

   client = all_clients;
   while (client != NULL) {
	   //Needed for calculating throughput
	   global_total_bytes_recv += client->total_bytes_recv;
	   global_total_resp_recv += client->total_resp_recv;

	   // Compute errors
	   global_errors += client->errors;

	   //Get nb_CRT from unfinished connections.
	   nb_CRT += (unsigned long long)client->nbRequestsOnIteration;

	   client = client->next;
   }

   global_avgCT = global_avgCT/nb_CT;

   global_avgCRT = global_avgCRT/nb_CRT;

   global_avgRT = global_avgRT/nb_RT;

}

/**
 * Print standards results of the load injection
 * Note: it has to be called after compute_stats()
 */
void print_stats() {
   DEBUG("----------------------------------------------------------------\n");
   DEBUG("-------------------------RESULTS--------------------------------\n");
   DEBUG("----------------------------------------------------------------\n");

   DEBUG("Total Time: %f sec. In microseconds: %.4lu us\n",
            (double) compare_time(&start_time, &stop_time)/1000000,
            compare_time(&start_time, &stop_time)
            );

   DEBUG("All next presented times are in microseconds(us).\n");

   DEBUG("Global bytes received: %llu bytes.\n", global_total_bytes_recv);
   DEBUG("Global response received: %llu bytes/sec.\n", global_total_resp_recv);

   DEBUG("global_avgCT:%.4Lf, global_avgRT:%.4Lf, global_avgCRT:%.4Lf, global_minCT:%lu, global_maxCT:%lu\n\n",
            global_avgCT,
            global_avgRT,
            global_avgCRT,
            global_minCT,
            global_maxCT);
}

/**
 * Close a client
 **/
static void delete_client(client_t *client) {
	assert(open_active == 1);

	// Potential concurrency problem?
	if (client->prev != NULL) {
		client->prev->next = client->next;
		if (client->next != NULL) {
			client->next->prev = client->prev;
		}
	}
	else {
		all_clients = client->next;
		if (client->next != NULL) {
			client->next->prev = NULL;
		}
	}
	DEBUG("Closing socket %d for client %d after state ST_ENDED.\n", client->number, client->fd);
	close(client->fd);

	//Needed for calculating throughput
	global_total_bytes_recv += client->total_bytes_recv;
	global_total_resp_recv += client->total_resp_recv;

	// Compute errors
	global_errors += client->errors;

	DEBUG("Client %d just finished.\n", client->number);
	free(client);
}
/**
 * Writing thread
 * Function used to inject load on the server.
 */

static void *writingFunc() {
   DEBUG("writingFunc going...\n");

   int err;
   states_t newClientState;

   client_t * client = all_clients;
   while (client != NULL) {
	   client->internalWriteStatus = DO_NOTHING;
	   client = client->next;
   }

   struct epoll_event events[MAX_CLIENTS];
   bzero(events, sizeof(events));

   //Main write loop
   uint64_t current_time_cycles;
   uint64_t last_time_cycles = 0;

   do {

      //Fill internalToWrite using the write circular buffer.
      while (1) {
		  err = get_circ(toWriteBuffer, (void **)&client);
		  if (err == CIRC_BUF_ERROR) {
			  //We stop filling if incomming buffer is empty.
			  break;
		  }
          DEBUG("New client for writer thread.\n");
		  client->internalWriteStatus = DO_WRITE;
		  if (client->fd >= 0) {
			  add_client_to_master_write_set(client);
		  } else {
              if (client->internalWriteStatus == DO_WRITE) {
                  if (client->state == ST_CONNECT) {//CONNECTING
                      if (!client->connect_in_progress) {
                          //We have to connect because it is the first time we connect for the loop or we had an error before
                          states_t newState = initialize_connect_client_to_server(client);

						  // Updating client info
						  if (client->state != newState) {
							  change_state(client, newState);

							  if (newState == ST_ENDED) {
								  delete_client(client);
							  }
						  }
                      }
                  }
              }
          }
      }

      // Epoll
      int nfds = epoll_wait(master_connect_epl, events, MAX_CLIENTS, 0);

      // Go through all available clients
      int i = 0;
      for (i = 0; i < nfds; i++) {
          client = (client_t *)events[i].data.ptr;

          // Should not be the case, EPOLL error
          if ((events[i].events & EPOLLERR) ||
              (events[i].events & EPOLLHUP) ||
              (!(events[i].events & EPOLLOUT))) {
              int error = 0;
              socklen_t errlen = sizeof(error);
              if (getsockopt(client->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0) {
                  PANIC("Receiving error while writing, error %s for client %d.\n", strerror(error), client->number);
              }
              PANIC("Receiving error while writing, unkown error for client %d.\n", client->number);
          }
          //If a client needs to connect or to write, let's do it.
          else if (client->internalWriteStatus == DO_WRITE) {

              if (client->state == ST_WRITING) {//WRITING
                  //generate request only if needed
                  if (client->new_request) {
                      BUILD_REQUEST(client);
                  }

                  DEBUG("Client %d just sent request\n", client->number);

                  newClientState = send_request(client);

                  //if we have gone to ST_READING, let's get out of the write thread, else, we stay here.
                  if (newClientState == ST_READING) {
                      remove_client_from_master_write_set(client);
                      client->internalWriteStatus = DO_NOTHING;
                      //put in read file (or connect if 3 threads)
                      if (put_circ(toReadBuffer, client) == CIRC_BUF_ERROR) {
                          if (open_active) {
                              PANIC("put_circ l.%d error, consider raising circular buffer size\n", __LINE__);
                          }
                          else {
                              PANIC("put_circ l.%d error. should be impossible\n", __LINE__);
                          }
                      }
                  }
                  else{
                      PANIC("After ST_WRITING must come ST_READING\n");
                  }

              }
              else if (client->state == ST_CONNECT) {//CONNECTING
                  if (client->connect_in_progress) {
                      finalize_connect_client_to_server(client);
                  }
              }
          }
      }

      rdtscll(current_time_cycles);

      if((current_time_cycles - last_time_cycles) > (3 * proc_freq)) {
          fprintf(stderr, "Nb requests : %llu\n", file_nb_req);
          fprintf(stderr, "Nb requests completed : %llu\n", file_nb_req_successful);

          last_time_cycles = current_time_cycles;
      }

   } while((current_time_cycles - start_time_cycles) < duration); //end while

   close(master_connect_epl);

   DEBUG("writingFunc end\n");
   pthread_exit(NULL);

}


/**
 * Reading thread
 * Function used to receive the answers from the server
 */
static void *readingFunc() {
   DEBUG("readingFunc going...\n");

   //a client temp ptr
   client_t * client;

   // return from circular buffer operations
   int err;

   //a var to store the new client state
   states_t newClientState;

   //Read set where to look for sockets are put
   int master_read_epl;
   struct epoll_event events[MAX_CLIENTS];
   struct epoll_event ev;

   bzero(events, sizeof(events));
   bzero(&ev, sizeof(ev));

   //select return value
   int num_ready;

   ///////////Init///////////

   //init read status
   client = all_clients;
   while (client != NULL) {
	   client->internalReadStatus = DO_NOTHING;
	   client = client->next;
   }

   ///////////Main read loop///////////
   uint64_t current_time_cycles;

   // In open loop, the number of cycle to wait between the last request and the
   // next one. Need to be decided each time a new request is created.
   uint64_t next_request_cycles;
   double next_seconds = 1.f / gaussrand(open_mean, open_var);
   if (open_active) {
       DEBUG("%f +- %f \t Next in %f\n", open_mean, open_var, next_seconds);
   }

   rdtscll(next_request_cycles);
   next_request_cycles += (next_seconds * (double)proc_freq);

   master_read_epl = epoll_create(MAX_CLIENTS);
   assert(master_read_epl != -1);

   do{
       //Fill internalToRead and master_read_set using the read circular buffer.
       while (1) {
           err = get_circ(toReadBuffer, (void **)&client);
           if (err == CIRC_BUF_ERROR) {
               //We stop filling if incomming buffer is empty.
               break;
           }
           client->internalReadStatus = DO_READ;
           ev.data.ptr = (void *)client;
           ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;

           assert(epoll_ctl(master_read_epl, EPOLL_CTL_ADD, client->fd, &ev) == 0);
       }

       num_ready = epoll_wait(master_read_epl, events, MAX_CLIENTS, 0);

       if (num_ready < 0) {
           perror("epoll");
           exit(0);
           //finish();
       }

       // Go through all available clients
       int i = 0;
       for (i = 0; i < num_ready; i++) {
           client = (client_t *)events[i].data.ptr;

           // Should not be the case, EPOLL error
           if ((events[i].events & EPOLLERR) ||
               (events[i].events & EPOLLHUP) ||
               (!(events[i].events & EPOLLIN))) {
               treat_error(client);
           }
           //Do read only if we need it.
           else if(client->internalReadStatus == DO_READ && events[i].events & EPOLLIN) {
               //read response
               newClientState = read_response(client);

               //Check if we have a changed state.
               if(newClientState != ST_READING) {

                   client->internalReadStatus = DO_NOTHING;
                   // No need to remove from this pool, the socket was closed in read_response (and thus remove).
                   if(newClientState == ST_CONNECT || newClientState == ST_WRITING ) {
                       //put in write (or connect if 3 threads) queue
                       if (put_circ(toWriteBuffer, client) == CIRC_BUF_ERROR) {
                           if (open_active) {
                               PANIC("put_circ l.%d error, consider raising circular buffer size\n", __LINE__);
                           }
                           else {
                               PANIC("put_circ l.%d error. should be impossible\n", __LINE__);
                           }
                       }//end if circ error
                   }//end if put in write file

                   // Client juste finished to work, delete it
                   else if (newClientState == ST_ENDED) {
					   delete_client(client);
                   }

               }//end if changed state

           }//end if do_read

       }//end for

       rdtscll(current_time_cycles);

       // Create new requests based on rdtscll difference
       if (open_active) {
           while (current_time_cycles > next_request_cycles) {
               // Create a new client for a new request
               // Concurrency issue?
               client_t *new_client = (client_t *)calloc(1, sizeof(client_t));
               assert(new_client != NULL);

               new_client->number = client_counter++;
               new_client->fd = -1;
               DEBUG("New client %d created at %s\n", client_counter - 1, getCurrentTime());
               new_client->state = ST_CONNECT;
               new_client->new_request = 1;

#if !MULTIPLE_INTERFACES
               init_server_target(new_client, host, port);
#endif
               // Put into circular buffer for next time
               put_circ(toWriteBuffer, new_client);

               // Insert in head into doubly linked list
               if (all_clients != NULL) {
                   all_clients->prev = new_client;
               }
               new_client->next = all_clients;
			   assert(new_client != new_client->next);
               new_client->prev = NULL;

               // Replace head of client list
               all_clients = new_client;

               // Compute time to next request
               double next_seconds = 1.f / gaussrand(open_mean, open_var);
               DEBUG("Next in %f\n", next_seconds);
               next_request_cycles += (next_seconds * (double)proc_freq);
           }
       }

   } while((current_time_cycles - start_time_cycles) < duration); //end while

   close(master_read_epl);
   pthread_exit(NULL);
}

/*********************** functions for slave mode **********************************/

/**
 * send stats response to the master.
 */
void send_stat_response(int masterOrderSocket) {

   int writeRet = 0;

   //Enables local address reuse
   if (setsockopt(masterOrderSocket, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr))
            < 0) {
      perror("setsockopt");
   }

   char response[RESPONSE_SIZE];
   bzero(response, RESPONSE_SIZE);

   /*
    * Format:
    * stopTime - startTime, global_bytes_throughput, global_req_throughput, global_avgCT, global_avgRT, global_avgCRT, global_minCT, global_maxCT, global_errors
    */
   int nb_to_w = snprintf(response, RESPONSE_SIZE, "%lu,%llu,%llu,%Lf,%Lf,%Lf,%lu,%lu,%lu\n", compare_time(&start_time, &stop_time), global_total_bytes_recv, global_total_resp_recv, global_avgCT, global_avgRT, global_avgCRT, global_minCT, global_maxCT, global_errors);

   assert(nb_to_w < RESPONSE_SIZE);

   DEBUG("Client will send response:\n\t %s",response);

   int written = 0;
   while(written < nb_to_w){
      writeRet = write(masterOrderSocket, response + written, nb_to_w - written);

      if (writeRet == 0) {
         PANIC("Error sending response to master (connection closed) .\n");
      }

      if (writeRet == -1) {
         PANIC("Error sending response to master.\n");
      }

      written += writeRet;
   }
   close(masterOrderSocket);

}

/**
 * Reset all data, counters, etc.
 */
void reset_stats() {
   //reset clients:
   if (open_active) {
	   client_t * client = all_clients;
	   client_t * next = NULL;
	   while (client != NULL) {
		   next = client->next;
		   free(client);
		   client = next;
	   }
   }
   else {
	   free(all_clients);
   }

   global_avgCT = 0;
   global_avgRT = 0;
   global_avgCRT = 0;

   nb_CRT = 0;
   nb_RT = 0;
   nb_CT = 0;

   global_total_bytes_recv = 0;
   global_total_resp_recv = 0;

   global_nbRequestsTaken = 0;
   global_maxCT = 0;
   global_minCT = 0;

   file_nb_req = 0;
   file_nb_req_successful = 0;
   file_nb_reads = 0;
}

//init the socket tht will accept the master connections.
void initListenMasterSocket() {

   // Receiving socket
   // Initializing the server
   masterListenOrderSocket = socket(AF_INET,SOCK_STREAM,0);

   if(masterListenOrderSocket<0){
      perror("cannot create master listen order socket");
      abort();
   }

   int val = 1;
   if (setsockopt(masterListenOrderSocket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val))
            < 0) {
      perror("setsockopt: ");
      exit(1);
   }

   server_addr.sin_family = AF_INET;
   server_addr.sin_port = htons(waitingMasterPort);
   server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

   if ((bind(masterListenOrderSocket, (struct sockaddr*) &server_addr, sizeof(struct sockaddr)))
            < 0) {
      perror("Bind: ");
      return;
   }

   if (listen(masterListenOrderSocket, 50000)<0) {
      perror("Listen : ");
      return;
   }

   DEBUG("Listen on 0.0.0.0:%d\n", waitingMasterPort);

   assert(gethostname(name, 255) == 0);
   my_hostname = atoi(name+3);
}

//Wait connection from the master:
int waitMasterConnection() {

   socklen_t length = sizeof(struct sockaddr);
   int sock = accept(masterListenOrderSocket, (struct sockaddr *)&server_addr, &length);

   if (sock>=0) {
      //Filling slaves_stats with the incoming data :
      struct sockaddr_in csin;
      unsigned int size = sizeof(csin);
      getpeername(sock, (struct sockaddr*)&csin, &size);

      //Master id
      DEBUG("master %s:%d is connected.\n", inet_ntoa(csin.sin_addr), htons(csin.sin_port));
   }
   else {
      perror("Master connection failed");
      abort();
   }

   return sock;

}

//Wait order from the master on masterOrderSocket socket:
int waitMasterOrder(int masterOrderSocket, char* req) {

   char buffer[MASTER_ORDER_BUFFER_SIZE];
   bzero(buffer, MASTER_ORDER_BUFFER_SIZE);

   // Reading message until \n
   int rd = 0;
   int totlaLength = 0;
   do {
      DEBUG("do while before read...\n");

      rd = read(masterOrderSocket, buffer+totlaLength, MASTER_ORDER_BUFFER_SIZE - totlaLength);

      DEBUG("do while after read...\n");

      if (rd == 0) {
         PRINT_ALERT("Connection closed and master order not full.\n");
         exit(-1);
      }
      else if (rd == -1) {
         PRINT_ALERT("Error while reading on master socket.\n");
         exit(-1);
      }

      totlaLength += rd;

      if (buffer[totlaLength - 1] == '\n') {
         buffer[totlaLength - 1] = '\0';
	 break;
      }

   } while (1);

   assert(totlaLength < MASTER_ORDER_BUFFER_SIZE);

   DEBUG("Received data: %s\n", buffer);

   /** params:
	* - slave_number
	* - reportingAddress
	* - host
	* - port
	* - nb_clients
	* - nb_iterations
	* - nb_msg_per_connection
	* - delay
	* - slaves_interval_avg_percent
	* - fastcgi_base_path
	* - open_active
	* - open_mean
	* - open_var
	*/

   const char delimiters[] = ",";
   char *token;

   req = strndup(buffer, MASTER_ORDER_BUFFER_SIZE-1);

   token = strtok(req, delimiters);
   slave_num = atoi(token);

   token = strtok(NULL, delimiters);
   reportingAddress = token;

   token = strtok(NULL, delimiters);
   host = token;

   token = strtok(NULL, delimiters);
   port = atoi(token);
   is_unix = host[0] == '/' && port == 0;

   token = strtok(NULL, delimiters);
   nb_clients = atoi(token);

   token = strtok(NULL, delimiters);
   duration = atoi(token) * proc_freq;

   token = strtok(NULL, delimiters);
   nb_msg_per_connection = atoi(token);

   token = strtok(NULL, delimiters);
   delay = atoi(token);

   token = strtok(NULL, delimiters);
   fastcgi_base_path = token;

   token = strtok(NULL, delimiters);
   open_active = atoi(token);

   token = strtok(NULL, delimiters);
   open_mean = atof(token);

   token = strtok(NULL, delimiters);
   open_var = atof(token);

   DEBUG( "Parameters: slave_num: %d, "
		  "reportingAddress: %s, "
		  "host:%s, "
		  "port:%d, "
		  "nb_clients:%d, "
		  "duration:%llu, "
		  "nb_msg_per_connection:%d, "
		  "delay:%d, "
		  "fastcgi_base_path:%s"
		  "open_active:%d, "
		  "open_mean:%f, "
		  "open_var:%f, "
		  "\n",
		  slave_num, reportingAddress, host, port, nb_clients, duration,
		  nb_msg_per_connection, delay, fastcgi_base_path,
		  open_active, open_mean, open_var);

   return 1;

}

//Close sockets of clients which still have request to do after bench time has expired.
void clean_remaing_clients_fd(){
   client_t *client;

   client = all_clients;
   while (client != NULL) {
	   close(client->fd);
	   client = client->next;
   }
}

/*
 * Main function
 */
int main(int argc, char** argv) {

   DEBUG("compiled at: %s %s\n",__TIME__ , __DATE__ );

   int masterOrderSocket;

   //GET PARAMETERS
   init_parameters();

   /* Clients now need to be started in a daemon mode */
   if (argc != 3 && argc != 2) {
      printf("Usage: %s <port> [file]\n", argv[0]);
      exit(-1);
   }

#if UNIQUE_FILE_ACCESS_PATTERN
   if(argc == 3){
      int dir_len = strlen(DEFAULT_DIR);
      int file_len = strlen(argv[2]);

      file = malloc((dir_len + file_len +1) * sizeof(char));
      strncpy(file, DEFAULT_DIR, dir_len);
      strncpy(file+dir_len, argv[2], file_len);
      file[dir_len+file_len] = 0;

   }
#endif

   fprintf(stderr,"********** System Info **********\n");
   do_proc_info(stderr, "/proc/sys/net/ipv4/tcp_tw_recycle");
   do_proc_info(stderr, "/proc/sys/net/ipv4/tcp_fin_timeout");
   do_proc_info(stderr, "/proc/sys/net/ipv4/ip_local_port_range");
   do_proc_info(stderr, "/proc/sys/net/ipv4/tcp_sack");
   do_proc_info(stderr, "/proc/sys/net/ipv4/tcp_timestamps");
   do_proc_info(stderr, "/proc/sys/net/core/rmem_max");
   fprintf(stderr,"*********************************\n\n");

#if SPECWEB99_FILE_ACCESS_PATTERN
   fprintf(stderr,"target file: %s\n", "SpecWeb99");
#elif SPECWEB05_FILE_ACCESS_PATTERN
   fprintf(stderr,"target file: %s\n", "SpecWeb05");
#elif SPECWEB09_BANKING_WORKLOAD
   fprintf(stderr,"target file: %s\n", "SpecWeb09 Banking Workload");
#elif UNIQUE_FILE_ACCESS_PATTERN
   fprintf(stderr,"target file: %s\n", file);
#else
   PRINT_ALERT("Unknown access file protocol\n");
   exit(EXIT_FAILURE);
#endif

#ifdef USE_RST
   fprintf(stderr,"## Using RST flag for closing connection ##\n\n");
#endif

   proc_freq = computeCPUhz();
   fprintf(stderr,"Running ... \n\n");

#if CLOSE_AFTER_REQUEST
   fprintf(stderr, "--- WARNING: Close after request ... ---\n");
#endif

   waitingMasterPort = atoi(argv[1]);

   /* Registering the handler, catching
    SIGPIPE signals */
   signal(SIGPIPE, sigpipe_handler);
   signal(SIGTERM, sigterm_handler);

   initListenMasterSocket();

   assert(masterListenOrderSocket>=0);

   while (1) {//deamon loop

      DEBUG("---------------------------------------------------------------\n");
      DEBUG("Waiting master orders.\n");

      //Accept master connection
      masterOrderSocket = waitMasterConnection();

      //Wait and read master orders
      char* req = NULL;
      if (waitMasterOrder(masterOrderSocket, req) < 0) {
         //if order fails, we continue.
         close(masterListenOrderSocket);
         continue;
      }

      // Print parameters
      DEBUG("Parameters: host:%s, port:%d, nb_clients:%d, duration:%llu, nb_msg_per_connection:%d, delay:%d\n",
               host, port, nb_clients, duration, nb_msg_per_connection, delay);
	  DEBUG("            open_active:%d, open_mean:%f, open_var:%f\n",
               open_active, open_mean, open_var);
      DEBUG("-----------------------------------------------------\n");

      //CHECK PARAMETERS
      check_parameters();

      //INIT PHASE
      DEBUG("Initalisation phase...\n");

      init_clients();

#if MULTIPLE_INTERFACES && LOAD_BALANCING
      memset(interfaces_pending, 0, sizeof(int)*NB_ITFS);
#endif

      master_connect_epl = epoll_create(MAX_CLIENTS);
      assert(master_connect_epl != -1);

      //Create circular buffers.
      toReadBuffer = malloc(sizeof(circularBuffer));
      toWriteBuffer = malloc(sizeof(circularBuffer));

      //Init circular buffers.
	  if (open_active) {
		  toReadBuffer = open_circ(toReadBuffer, 2 * MAX_CLIENTS);
		  toWriteBuffer = open_circ(toWriteBuffer, 2 * MAX_CLIENTS);
	  }
	  else {
		  toReadBuffer = open_circ(toReadBuffer, nb_clients);
		  toWriteBuffer = open_circ(toWriteBuffer, nb_clients);
	  }

      if (toReadBuffer==NULL || toReadBuffer==NULL) {
         printf("malloc error while creating circular buffers.\n");
         exit(-1);
      }

	  //Put all clients in the write queue because they will have to connect.
	  client_t * client = all_clients;
	  while (client != NULL) {
		  put_circ(toWriteBuffer, client);
		  client = client->next;
	  }

      if (duration < 1) {
         printf("Test duration too low.\n");
         exit(0);
      }

      //||||-Probe main start-||||//
      assert(gettimeofday(&start_time,NULL) == 0);

      rdtscll(start_time_cycles);
      //|||||||||||||||//

      DEBUG_TMP("[Slave %d] Clients start at : %s\n", slave_num, getCurrentTime());

      //The 2 following threads are the main core of the load injector.
      pthread_create(&reading_thread, NULL, readingFunc, NULL);
      pthread_create(&writing_thread, NULL, writingFunc, NULL);

      pthread_join(writing_thread, NULL);
      pthread_join(reading_thread, NULL);

      //||||-Probe main stop-||||//
      assert(gettimeofday(&stop_time,NULL) == 0);
      //|||||||||||||||//

      //printf("%lu\t", compare_time(&start_time, &stop_time) );

      DEBUG("-----------------------------------------------------\n");
      DEBUG("All clients have ended: %s\n", getCurrentTime());

      /* Stats */
      compute_stats();

      //send stat to the master
      send_stat_response(masterOrderSocket);

      clean_remaing_clients_fd();

      //print_stats();

      if (global_total_resp_recv == 0) {
         PRINT_ALERT("WARNING: No response received in this iteration\n");
         client_t * client;
         int nb_have_written = 0;
         int nb_connecting = 0;
         float nb_connect_attempts_in_progress = 0;
         int nb_have_done_nothing = 0;

		 client = all_clients;
		 while (client != NULL) {

			 if (client->connect_in_progress){
				 nb_connecting++;
				 nb_connect_attempts_in_progress += (client->nb_connect_attempts ? client->nb_connect_attempts : 1);
			 }
			 else if (client->bytes_write) {
				 nb_have_written++;
			 }
			 else {
				 nb_have_done_nothing++;
			 }

			 client = client->next;
		 }

         PRINT_ALERT("\t%d clients have written but no answer.\n", nb_have_written);
         PRINT_ALERT("\t%d clients have trying to connect (avg attempts: %.2f %%).\n", nb_connecting, (nb_connecting)?(nb_connect_attempts_in_progress/nb_connecting):0);
         PRINT_ALERT("\t%d clients have done nothing since this iteration.\n", nb_clients - nb_have_written - nb_connecting);
      }
      else{
         printf("Nb request : %llu\n", file_nb_req);
         printf("Nb read syscalls : %llu\n", file_nb_reads);
         printf("Nb finish : %llu\n", file_nb_req_successful);
      }

      DEBUG("Clients end at : %s\n", getCurrentTime());
      fprintf(stderr,"-------------------------------------------------\n\n");

      reset_stats();

      //Init circular buffers.
      close_circ(toReadBuffer);
      close_circ(toWriteBuffer);

      free(toReadBuffer);
      free(toWriteBuffer);

      free(req);

      close(masterOrderSocket);

   }

   close(masterListenOrderSocket);

   //end read order loop.

   return 0;
}
