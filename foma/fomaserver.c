#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h> 
#include <limits.h>

#include <getopt.h>
#include "fomalib.h"

#define LINE_LIMIT 262144
#define FLOOKUP_PORT 8888
#define DIR_UP 1
#define UDP_MAX 65535
#define LINE_MAX_LENGTH 6000


char *usagestring = "Usage: FomaServer [-l med_limit] [-c cutoff] [-P port number] <analyzer.bin> <chain.bin> <spellcheckUnificado.bin>\n";
char *helpstring = "Starts spellcheck server on localhost"
        "Use with tcpClient to spellcheck data from stdin with finite state tranducers read from 3 files (pass in this order!):\n"
        "\t<analyzer.bin> :\t\tstrict analyzer that decides whether a word needs spell checking or not\n"
        "\t<chain.bin> :\t\t\tchain of normalizers that will to correct typical misspelings (e.g. '-nqui' -> '-nki', 'hua' -> 'wa')\n"
        "\t<spellcheckUnificado.bin> :\tactual spell checker that suggests similar words based on minimum edit distance\n"
        "Options:\n"
        "-h\t\tprint this help\n"
        "-l med_limit\tset maximum number of suggestions (default is 5)\n"
        "-c cutoff\tset maximum levenshtein distance for suggestions (default is 15)\n"
        "-P\t\tspecify port of server (default 8888)\n";

struct lookup_chain {
    struct fsm *net;
    struct apply_handle *ah;
    struct lookup_chain *next;
    struct lookup_chain *prev;
};

static struct fsm *analyzernet;
static struct apply_handle *ah;
static struct fsm *mednet;
static struct apply_med_handle *medh;

static struct fsm *net;
static fsm_read_binary_handle fsrh;
static struct lookup_chain *chain_head, *chain_tail, *chain_new, *chain_pos;
static int numnets = 0, echo = 1, apply_alternates = 1, index_flag_states = 0, index_cutoff = 0, index_mem_limit = INT_MAX, index_arcs = 0, direction = DIR_UP, buffered_output = 1, results, port_number = FLOOKUP_PORT;
//static char  *server_address = NULL, *line, *serverstring = NULL;
struct sockaddr_in serv_addr;

static FILE *INFILE;

static char *(*applyer)() = &apply_up; /* Default apply direction = up */
// static void handle_line(char *line, int connfd);
static char *handle_line(char *line);
static char *get_next_line();
char* concat_outstr(char *s1, char *s2);
char* concat_med(char *s1, char *s2, char *s3);
char* concat(char *s1, char *s2);


char *result, *separator = "\t";
int firstIncorrWord = 1;
int firstCorrWord = 1;
char *corr;
char *incorr;
int indexIncorrectWords = 0;
int indexCorrectWords = 0;

//    static char *line;
//  static void server_init();

int main(int argc, char *argv[]) {

    int opt = 1, sortarcs = 1;
    char *chainname, *analyzername, *medname;
    struct fsm *net;
    INFILE = stdin;

    extern g_med_limit;
    extern g_med_cutoff;

    extern g_med_limit;
    extern g_med_cutoff;

    while ((opt = getopt(argc, argv, "l:c:P:h")) != -1) {
        switch (opt) {
            case 'l':
                if (atoi(optarg) == 0) {
                    printf("Maximum number of suggestions can not be zero! Using default value (5).\n");
                    //because med search won't terminate with g_med_limit = 0*/
                    break;
                } else {
                    g_med_limit = atoi(optarg);
                    break;
                }
            case 'c':
                g_med_cutoff = atoi(optarg);
                break;
            case 'h':
                printf("%s%s\n", usagestring, helpstring);
                exit(0);
            case 'P':
                port_number = atoi(optarg);
                break;
            default:
                fprintf(stderr, "%s", usagestring);
                exit(EXIT_FAILURE);
        }
    }

    /* get analyzer binary  */
    analyzername = argv[optind];
    analyzernet = fsm_read_binary_file(analyzername);
    if (analyzernet == NULL) {
        fprintf(stderr, "%s: %s\n%sprint -h for help\n", "File error", analyzername, usagestring);
        exit(EXIT_FAILURE);
    }

    ah = apply_init(analyzernet);

    //########################################//
    int listenfd = 0, connfd = 0;
    int byte_count;
    char sendBuff[1025];


    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof (serv_addr));
    memset(sendBuff, '0', sizeof (sendBuff));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port_number);

    // bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    /* Now bind the host address using bind() call.*/
    if (bind(listenfd, (struct sockaddr*) &serv_addr, sizeof (serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    } else {
        printf("started server on port %d\n", port_number);
    }

    listen(listenfd, 10);

    while (1) {
        connfd = accept(listenfd, (struct sockaddr*) NULL, NULL);
        if ( connfd < 0 ) {
            perror("ERROR on accept");
            return EXIT_FAILURE;
        }
        // fork child
        //int pid = fork();
        //if (pid < 0) {
        //    perror("ERROR could not fork");
        //    exit(1);
        //}
        //if (pid == 0) {
            /* This is the client process */
            //close(listenfd);
            do {
                byte_count = recv(connfd, sendBuff, 16384, 0);
                if (byte_count < 0) {
                  perror("ERROR reading from socket");
                  return EXIT_FAILURE;
                } else {
                  sendBuff[byte_count] = '\0';
                  char *line = concat(sendBuff, "");
                  char *outstring = handle_line(line);
                  //write(connfd, outstring, strlen(outstring));
                  if (write(connfd, outstring, strlen(outstring)) == -1) {
                    perror("ERROR writing to socket failed");
                    return EXIT_FAILURE;
                  }
                }
            } while (byte_count > 0);
            //exit(0);
        //} else {
        close(connfd);
        //}

    }

    close(connfd);
    sleep(1);
}

char *handle_line(char *s) {
    char *result, *tempstr, *outstr;
    int normalized = 0;
    char *line = concat(s, "");
    outstr = "";
    /* make sure the string is not empty */
    if (line[0] != '\0') {
        /* Apply analyzer.bin */
        result = apply_up(ah, line);
        /* if no result from analyzer, spell check this word with normalizer */
        if (result == NULL) {
          outstr = concat(line, "\t+?");
        }            /* word was recognized by analyzer.bin */
        else {
          /*Concat first result*/
          tempstr = concat(line, "\t");
          tempstr = concat(tempstr, result);
          tempstr = concat(tempstr, "\n");
          outstr = concat(outstr, tempstr);
          /*Concat the rest of the results (if there are more of them)*/
          while ((result = apply_up(ah,NULL)) != NULL && strlen(outstr) < LINE_MAX_LENGTH) {
            tempstr = concat(line, "\t");
            tempstr = concat(tempstr, result);
            tempstr = concat(tempstr, "\n");
            outstr = concat(outstr, tempstr);
          }
        }
    }
    printf("%d \n", strlen(outstr));
    return outstr;
}

// void handle_line(char *s, int connfd) {
//     char *result, *tempstr, *outstr;
//     int normalized=0;
//     char *line = concat(s,"");
//     
//     /* make sure string is not empty */
//     if(line[0] != '\0')
//     {
// 	/* Apply analyzer.bin */  
// 	result = apply_up(ah, line);
// 	
// 	/* if no result from analyzer, spell check this word with normalizer */
// 	if(result == NULL)
// 	{
// 	  /* apply chain.bin (normalizer) */
// 	    if (apply_alternates == 1) {
// 	     // printf("apply_alternates is true, line is %s\n",  line);
// 	      for (chain_pos = chain_head, tempstr = s;   ; chain_pos = chain_pos->next) {
// 		  result = applyer(chain_pos->ah, tempstr);
// 		  if (result != NULL) {
// 		      results++;
// 		      printf("%s:\n \t%s\n", line, result);
// 		      outstr = concat_outstr(line, result);
// 		     // write(connfd, outstr, strlen(outstr));
// 		      normalized=1;
// 		      while ((result = applyer(chain_pos->ah, NULL)) != NULL) {
// 			  results++;
// 			printf("%s:\n \t%s\n", line, result);
// 			   char *outstr1 = concat_outstr(line, result);
// 			   outstr = concat(outstr, outstr1);
// 		      }
// 		      break;
// 		  }
// 		  if (chain_pos == chain_tail) {
// 		      break;
// 		  }
// 	      }
// 	      if(normalized == 1){
// 		write(connfd, outstr, strlen(outstr));
// 	      }
// 	    } 
// 	    else {    
// 	      /* Get result from chain */
// 	      for (chain_pos = chain_head, tempstr = s;  ; chain_pos = chain_pos->next) {		
// 		  result = applyer(chain_pos->ah, tempstr);		
// 		  if (result != NULL && chain_pos != chain_tail) {
// 		      tempstr = result;
// 		      continue;
// 		  }
// 		  if (result != NULL && chain_pos == chain_tail) {
// 		      do {
// 			  results++;
// 			  printf("%s:\n \t%s\n", line, result);
// 			  char *outstr2 = concat_outstr(line, result);
// 			  outstr = concat(outstr, outstr2);
// 			  normalized=1;
// 		      } while ((result = applyer(chain_pos->ah, NULL)) != NULL);
// 		  }
// 		  if (result == NULL) {
// 		      /* Move up */
// 		      for (chain_pos = chain_pos->prev; chain_pos != NULL; chain_pos = chain_pos->prev) {
// 			  result = applyer(chain_pos->ah, NULL);
// 			  if (result != NULL) {
// 			      tempstr = result;
// 			      break;
// 			  }
// 		      }
// 		  }
// 		  if (chain_pos == NULL) {
// 		      break;
// 		  }
// 	      }
// 	      if(normalized == 1){
// 		write(connfd, outstr, strlen(outstr));
// 	      }
// 	  }
// 	  /* if no result from chain.bin (normalizer), use med search with spellcheckUnificado.bin */
// 	  if(normalized == 0){
// 		/*apply med search*/
// 		//printf("%s\n", line);
// 		char *outstr1 = concat(line, "\n");
// 		result = apply_med(medh, line);
// 		if (result == NULL) 
// 		{
// 		    outstr = concat(outstr1, "\t???\n");
// 		    write(connfd, outstr, strlen(outstr));
// 		} 
// 		else 
// 		{
//  		    char *outstr2 = concat_med("\t", result, "\n");
//  		    char *outstr3 = concat_med("\t", apply_med_get_instring(medh), "\n");
// 		    // get cost
//  		    char costStr[20];
//  		    int cost = apply_med_get_cost(medh);
//  		    sprintf(costStr,"%d",cost);
// 		    char *outstr4 = concat_med("\tCost[f]: ", costStr, "\n\n");
// 		    char *outstr5 = concat_med(outstr1, outstr2, outstr3);
// 		    char *firstout = concat(outstr5, outstr4);
// 		    
// 		    char *secondout ="";
// 		    while ((result = apply_med(medh,NULL)) != NULL) {
// 		      char *secondout1 = concat_med("\t", result, "\n");
// 		      char *secondout2 = concat_med("\t", apply_med_get_instring(medh), "\n");
// 		       // get cost
// 		      char costStr[20];
// 		      int cost = apply_med_get_cost(medh);
// 		      sprintf(costStr,"%d",cost);
// 		      char *secondout3 = concat_med("\tCost[f]: ", costStr, "\n\n");
// 		      char *secondout4 = concat_med(secondout1, secondout2, secondout3);
// 		      secondout = concat(secondout, secondout4);
// 		    }
// 		    outstr = concat(firstout, secondout);
// 		    write(connfd, outstr, strlen(outstr));
// 		}
// 	    }
// 	  
// 	}
// 	/* word was recognized by analyzer.bin */
// 	else{
// 	  //printf("%s:\n \t%s\n", line, "--");
// 	  outstr = concat(line, "--");
// 	  write(connfd, outstr, strlen(outstr));
// 	}
//     }
// }

char* concat_outstr(char *s1, char *s3) {
    char *s2 = ":\n \t";
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    size_t len3 = strlen(s3);
    char *result = malloc(len1 + len2 + len3 + 1); //+1 for the zero-terminator
    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2); //+1 to copy the null-terminator
    memcpy(result + len1 + len2, s3, len3 + 1);
    return result;
}

char* concat_med(char *s1, char *s2, char *s3) {

    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    size_t len3 = strlen(s3);
    char *result = malloc(len1 + len2 + len3 + 1); //+1 for the zero-terminator
    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2); //+1 to copy the null-terminator
    memcpy(result + len1 + len2, s3, len3 + 1);
    return result;
}

char* concat(char *s1, char *s2) {

    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char *result = malloc(len1 + len2 + 1); //+1 for the zero-terminator
    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2 + 1); //+1 to copy the null-terminator
    return result;
}




