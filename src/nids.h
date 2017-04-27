/*
  Copyright (c) 1999 Rafal Wojtczuk <nergal@7bulls.com>. All rights reserved.
  See the file COPYING for license details.
*/

#ifndef _NIDS_NIDS_H
# define _NIDS_NIDS_H

# include <sys/types.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
# include <netinet/ip.h>
# include <netinet/tcp.h>
# include <pcap.h>

# ifdef __cplusplus
extern "C" {
# endif

# define NIDS_MAJOR 1
# define NIDS_MINOR 24

enum
{
  NIDS_WARN_IP = 1,//IP���ݰ��쳣
  NIDS_WARN_TCP,//TCP���ݰ��쳣
  NIDS_WARN_UDP,//UDP���ݰ��쳣
  NIDS_WARN_SCAN//��ʾ��ɨ�蹥������
};

enum
{
  NIDS_WARN_UNDEFINED = 0,/*��ʾδ����*/
  NIDS_WARN_IP_OVERSIZED,/*��ʾIP���ݰ�����*/
  NIDS_WARN_IP_INVLIST,/*��ʾ��Ч����Ƭ����*/
  NIDS_WARN_IP_OVERLAP,/*��ʾ�����ص�*/
  NIDS_WARN_IP_HDR,/*��ʾ��ЧIP�ײ���IP���ݰ������쳣*/
  NIDS_WARN_IP_SRR,/*��ʾԴ·��IP���ݰ�*/
  NIDS_WARN_TCP_TOOMUCH,/*��ʾtcp���ݸ���̫�࣬��Ϊ��libnids����ͬһʱ�̲����tcp�������ֵΪtcp���Ӳ����Ĺ�ϣ����3/4  */
  NIDS_WARN_TCP_HDR,/*��ʾ��ЧTCP�ײ���TCP���ݰ������쳣*/
  NIDS_WARN_TCP_BIGQUEUE,/*��ʾTCP���յĶ������ݹ���*/
  NIDS_WARN_TCP_BADFLAGS/*��ʾ������*/
};

# define NIDS_JUST_EST 1/*��ʾtcp���ӽ���*/
# define NIDS_DATA 2  /*��ʾ�������ݵ�״̬*/
# define NIDS_CLOSE 3 /*��ʾtcp���������ر�*/
# define NIDS_RESET 4 /*��ʾtcp���ӱ����ùر�*/
# define NIDS_TIMED_OUT 5  /*��ʾ���ڳ�ʱtcp���ӱ��ر�*/
# define NIDS_EXITING   6	/* ��ʾ���ڳ�ʱtcp���ӱ��ر�nids is exiting; last chance to get data */

# define NIDS_DO_CHKSUM  0
# define NIDS_DONT_CHKSUM 1

struct tuple4
{
  u_short source;
  u_short dest;
  u_int saddr;
  u_int daddr;
};/*��������һ����ַ�˿ڶԣ�����ʾ���ͷ�IP�Ͷ˿��Լ����շ�IP�Ͷ˿�*/

struct half_stream
{
  char state;
  char collect;
  char collect_urg;

  char *data;
  int offset;
  int count;
  int count_new;
  int bufsize;
  int rmem_alloc;

  int urg_count;
  u_int acked;
  u_int seq;
  u_int ack_seq;
  u_int first_data_seq;
  u_char urgdata;
  u_char count_new_urg;
  u_char urg_seen;
  u_int urg_ptr;
  u_short window;
  u_char ts_on;
  u_char wscale_on;
  u_int curr_ts; 
  u_int wscale;
  struct skbuff *list;
  struct skbuff *listtail;
};

struct tcp_stream
{
  struct tuple4 addr;
  char nids_state;
  struct lurker_node *listeners;
  struct half_stream client;
  struct half_stream server;
  struct tcp_stream *next_node;
  struct tcp_stream *prev_node;
  int hash_index;
  struct tcp_stream *next_time;
  struct tcp_stream *prev_time;
  int read;
  struct tcp_stream *next_free;
  void *user;
};

/*����libnids��һЩȫ�ֲ�����Ϣ*/
struct nids_prm
{
  int n_tcp_streams;/*��ʾ�������С,�˹������������tcp_stream���ݽṹ,*/
  int n_hosts;/*��ʾ���ip��Ƭ��Ϣ�Ĺ�����Ĵ�С*/
  char *device;
  char *filename;/*�����洢�������ݲ����ļ�.����������ļ�,���ͬʱ��Ӧ�����ó�ԱdeviceΪnull,Ĭ��ֵΪNULL*/
  int sk_buff_size;/*��ʾ���ݽṹsk_buff�Ĵ�С.���ݽṹsk_buff��linux�ں���һ����Ҫ�����ݽṹ,�������������ݰ����в�����*/
  int dev_addon;/*��ʾ�����ݽṹsk_buff����������ӿ�����Ϣ���ֽ���,�����-1(Ĭ��ֵ),��ôlibnids����ݲ�ͬ������ӿڽ�������*/
  void (*syslog) ();//����ָ��,Ĭ��ֵΪnids_syslog()����.��syslog�п��Լ�����ֹ���,��:����ɨ�蹥��
  /*������������Ϊnids_syslog(int type,int errnum,struct ip_header * iph,void *data)*/
  int syslog_level;//��ʾ��־�ȼ�,Ĭ��ֵΪLOG_ALERT.
  int scan_num_hosts;//��ʾ�洢�˿�ɨ����Ϣ�Ĺ�����Ĵ�С
  int scan_delay;//��ʾ��ɨ������,���˿�ɨ��ļ��ʱ��
  int scan_num_ports;//��ʾ��ͬԴ��ַ����ɨ���tcp�˿���Ŀ
  void (*no_mem) (char *);//��libnids�����ڴ����ʱ������
  int (*ip_filter) ();//����ָ��,�˺���������������ip���ݰ�,����ip���ݰ�����ʱ,�˺���������.Ĭ��ֵΪnids_ip_filter,�ú����Ķ������£�
  /*static int nids_ip_filter(struct ip * x,int len) */
  char *pcap_filter;//��ʾ���˹���
  int promisc;//��ʾ����ģʽ,��0Ϊ����ģʽ,����Ϊ�ǻ���ģʽ,Ĭ��ֵΪ1
  int one_loop_less;
  int pcap_timeout;
  int multiproc;
  int queue_limit;
  int tcp_workarounds;
  pcap_t *pcap_desc;
};

struct tcp_timeout
{
  struct tcp_stream *a_tcp;
  struct timeval timeout;
  struct tcp_timeout *next;
  struct tcp_timeout *prev;
};

int nids_init (void);
void nids_register_ip_frag (void (*));
void nids_unregister_ip_frag (void (*));
void nids_register_ip (void (*));
void nids_unregister_ip (void (*));
void nids_register_tcp (void (*));
void nids_unregister_tcp (void (*x));
void nids_register_udp (void (*));
void nids_unregister_udp (void (*));
void nids_killtcp (struct tcp_stream *);
void nids_discard (struct tcp_stream *, int);
int nids_run (void);
void nids_exit(void);
int nids_getfd (void);
int nids_dispatch (int);
int nids_next (void);
void nids_pcap_handler(u_char *, struct pcap_pkthdr *, u_char *);
struct tcp_stream *nids_find_tcp_stream(struct tuple4 *);
void nids_free_tcp_stream(struct tcp_stream *);

int nids_dpdk_init();
int nids_dpdk_run(int argc,char **argv);

extern struct nids_prm nids_params;
extern char *nids_warnings[];
extern char nids_errbuf[];
extern struct pcap_pkthdr *nids_last_pcap_header;
extern u_char *nids_last_pcap_data;
extern u_int nids_linkoffset;
extern struct tcp_timeout *nids_tcp_timeouts;

struct nids_chksum_ctl {
	u_int netaddr;
	u_int mask;
	u_int action;
	u_int reserved;
};
extern void nids_register_chksum_ctl(struct nids_chksum_ctl *, int);

# ifdef __cplusplus
}
# endif

#endif /* _NIDS_NIDS_H */
