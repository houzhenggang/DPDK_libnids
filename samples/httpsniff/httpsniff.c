//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>
#include "nids.h"
#include "nids.h"

#include "openssl/md5.h"
#include "httpsniff.h"
#include "ver.h"

int debug = 0;
unsigned short ports[16] = {80,0};
char suffix[32][32] = {"doc","docx","xls","xlsx","ppt","pptx","wps","pdf","html","htm","txt","ceb","zip","rar"};
unsigned int filesize_min = 0;
unsigned int filesize_max = 0;
char save_dir[256] = "upload";
struct conn_list g_hlist[HASHSIZE];
unsigned int g_hlist_entry = 0;
#define PIDFILE "/var/run/httpsniff.pid"

struct conn_ctx *conn_ctx_new(int size)
{
	struct conn_ctx *e;

	e = (struct conn_ctx *)malloc(sizeof(struct conn_ctx));
	if (!e)
		return NULL;

	memset(e,0,sizeof(struct conn_ctx));

	e->file_fd = -1;
	INIT_LIST_HEAD(&e->list);

	return e;
}

void conn_ctx_free(struct conn_ctx *e)
{
	if (!e)
		return;
	if (e->file_fd != -1) {
		close(e->file_fd);
		// �ļ��洢��Ȼ��������ɾ����
		unlink(e->cur_filepath);
	}

	free(e);

	return;
}

int io_write(int fd,void *data,int inl)
{
	int ret;
	int left_len = inl;
	char *p = (char *)data;

	if (inl < 0)
		return -1;

	while(left_len > 0) {
		ret = write(fd,p,left_len);
		if (ret < 0)
			return ret;
		left_len -= ret;
		p += ret;
	}

	return inl;
}

struct conn_ctx *lookup_by_tuple4(unsigned int src,unsigned int dst,
							unsigned short sport,unsigned short dport)
{
	struct conn_ctx *find=NULL;
	struct conn_ctx *e;
	struct list_head *pos;
	unsigned int hash=hash_by_tuple4(src,dst,sport,dport);

	pthread_rwlock_rdlock(&g_hlist[hash].lock);
	list_for_each(pos,&g_hlist[hash].list) {
		e = list_entry(pos,struct conn_ctx,list);
		if (e->src==src && e->sport==sport 
			&& e->dst==dst && e->dport==dport) {
			find = e;
			break;
		}
	}
	pthread_rwlock_unlock(&g_hlist[hash].lock);

	return find;
}

char *http_trans_buf_has_patt(char *a_buf, int a_len,
						char *a_pat, int a_patlen)
{
	int i = 0;
	for ( ; i <= ( a_len - a_patlen ); i++ ) {
		if (a_buf[i] == a_pat[0]) {
			if (memcmp(&a_buf[i], a_pat, a_patlen) == 0)
				return &a_buf[i];
		}
	}
	return NULL;
}

/*
 * Find first occurrence of searchstr in s, limited by slen.
 * A 'safe' version of strstr that won't read past end of buffer s
 * in cases that s is not NULL terminated.
 */
const char *SnortStrnStr(const char *s, int slen, const char *searchstr)
{
    char ch, nc;
    int len;
    if (!s || (slen == 0) || !*s || !searchstr)
        return NULL;

    if ((ch = *searchstr++) != 0)
    {
        len = strlen(searchstr);
        do
        {
            do
            {
                if ((nc = *s++) == 0)
                {
                    return NULL;
                }
                slen--;
                if (slen == 0)
                    return NULL;
            } while (nc != ch);
            if (slen - len < 0)
                return NULL;
        } while (memcmp(s, searchstr, len) != 0);
        s--;
    }
    return s;
}

/*
 * Find first occurrence of substring in s, ignore case.
*/
char *SnortStrcasestr(char *s, int slen, char *substr)
{
    char ch, nc;
    int len;

    if (!s || (slen == 0) || !*s || !substr)
        return NULL;

    if ((ch = *substr++) != 0)
    {
        ch = tolower((char)ch);
        len = strlen(substr);
        do
        {
            do
            {
                if ((nc = *s++) == 0)
                {
                    return NULL;
                }
                slen--;
                if(slen == 0)
                    return NULL;
            } while ((char)tolower((uint8_t)nc) != ch);
            if(slen - len < 0)
                return NULL;
        } while (strncasecmp(s, substr, len) != 0);
        s--;
    }
    return s;
}

void process_http(struct tcp_stream *a_tcp)
{
	int i,hash,hdr_len,n_content_len,n_boundary_len;
	char *hdr_crlfcrlf,*content_length,*content_type,*referer,*boundary;
	char *p,*p1,*p2,ch_tmp;
	struct conn_ctx *e = NULL;
	struct half_stream *hlf=&a_tcp->server;
	int left_len = hlf->count-hlf->offset;
	char *p_start = hlf->data;
	char *p_cur = p_start;
	time_t t_now;
	struct tm *tmptr;

	e = lookup_by_tuple4(a_tcp->addr.saddr,a_tcp->addr.daddr,
		a_tcp->addr.source,a_tcp->addr.dest);

	if (!e) {	// δ�ҵ�
		if (hlf->count-hlf->offset < 5) {
			// ����̫�٣������������ݣ��ȴ����յ������
			nids_discard(a_tcp,0);
			goto quit;
		}
		// ����POST���϶�����������Ҫ��
		if (strncmp(hlf->data,"POST",4) != 0) {
			a_tcp->server.collect--;
			//nids_free_tcp_stream(a_tcp) // ����ֱ��free��
			goto quit;
		}
		hdr_crlfcrlf = http_trans_buf_has_patt(hlf->data,hlf->count-hlf->offset,"\r\n\r\n",4);
		if (!hdr_crlfcrlf) {
			// httpͷ����Ϣ��δ������ȫ�������������ݣ��ȴ����յ������
			nids_discard(a_tcp,0);
			goto quit;
		}

		// �����http��ͷ����Ϣ�Ѿ�ȫ���յ�----------------------------
		hdr_len = hdr_crlfcrlf-hlf->data+4;
		content_length = SnortStrcasestr(hlf->data,hdr_crlfcrlf-hlf->data,"Content-Length: ");
		if (!content_length) {
			// httpͷ��û�д˱�ʶ���򲻿������ϴ��ļ���
			a_tcp->server.collect--;
			//nids_free_tcp_stream(a_tcp) // ����ֱ��free��
			goto quit;
		}
		content_length += 16;
		p = content_length;
		while((*p!=' ') && (*p!='\r'))
			p++;
		ch_tmp = *p;
		*p = 0;
		n_content_len = atoi(content_length);
		*p = ch_tmp;
		if (filesize_min!=0 && n_content_len<filesize_min) {
			// �ļ���С���Ȳ������õķ�Χ�ڣ�����ֻ�Ǵ���ж�һ��
			// content_length�������ϴ������ݵ��ܳ��ȣ������ļ����ݼ�����һЩ������Ϣ��
			// �������յ��ļ����ȱ�������ȫ�յ��ļ������֪��
			a_tcp->server.collect--;
			goto quit;
		}

		content_type = SnortStrcasestr(hlf->data,hdr_crlfcrlf-hlf->data,"Content-Type");
		if (!content_type) {
			// httpͷ��û�д˱�ʶ���򲻿������ϴ��ļ���
			a_tcp->server.collect--;
			//nids_free_tcp_stream(a_tcp) // ����ֱ��free��
			goto quit;
		}
		boundary = http_trans_buf_has_patt(content_type,hdr_crlfcrlf-content_type,"boundary=",9);
		if (!boundary) {
			// httpͷ��û�д˱�ʶ���򲻿������ϴ��ļ���
			a_tcp->server.collect--;
			//nids_free_tcp_stream(a_tcp) // ����ֱ��free��
			goto quit;
		}
		boundary += 9;
		p = http_trans_buf_has_patt(boundary,hdr_crlfcrlf-boundary+4,"\r\n",2);
		ch_tmp = p[0];
		p[0] = 0;
		n_boundary_len = strlen(boundary);

		P_DEBUG("boundary: %s\n",boundary);
		p[0] = ch_tmp;

		// add new one
		e = conn_ctx_new(hlf->count_new);
		if (!e) {
			a_tcp->server.collect--;
			//nids_free_tcp_stream(a_tcp) // ����ֱ��free��
			goto quit;
		}
		e->start_time = time(NULL);
		// ע������������ȡURL�Ĳ�������ctx����֮��
		// ��ȡPOST��URL
		p1 = hlf->data + 5;
		while(p1[0] == ' ') p1++;
		p2 = p1;
		while(p2[0] != ' ' && p2[0] != '\r') p2++;
		ch_tmp = p2[0];
		p2[0] = 0;
		strncpy(e->post_url,p1,sizeof(e->post_url)-1);
		p2[0] = ch_tmp;

		// ��ȡref URL
		referer = SnortStrcasestr(hlf->data,hdr_crlfcrlf-hlf->data,"Referer: ");
		if (referer) {
			referer += 9;
			p = http_trans_buf_has_patt(referer,hdr_crlfcrlf-referer+4,"\r\n",2);
			ch_tmp = p[0];
			p[0] = 0;
			strncpy(e->ref_url,referer,sizeof(e->ref_url)-1);
			p[0] = ch_tmp;
		}
		else {
			strcpy(e->ref_url,"no ref url");
		}

		// test, ��ȡUA
		referer = SnortStrcasestr(hlf->data,hdr_crlfcrlf-hlf->data,"User-Agent: ");
		if (referer) {
			referer += 12;
			p = http_trans_buf_has_patt(referer,hdr_crlfcrlf-referer+4,"\r\n",2);
			ch_tmp = p[0];
			p[0] = 0;
			strncpy(e->ua,referer-12,sizeof(e->ua)-1);
			p[0] = ch_tmp;
		}
		else {
			strcpy(e->ua,"no user agent");
		}


		e->state = STATE_READ_DATA;
		boundary[n_boundary_len] = 0;
		strncpy(e->boundary1+4,boundary,sizeof(e->boundary1)-7);
		memcpy(e->boundary1,"\r\n--",4);

		e->src = a_tcp->addr.saddr;
		e->dst = a_tcp->addr.daddr;
		e->sport = a_tcp->addr.source;
		e->dport = a_tcp->addr.dest;
		hash = hash_by_tuple4(a_tcp->addr.saddr,a_tcp->addr.daddr,
			a_tcp->addr.source,a_tcp->addr.dest);

		pthread_rwlock_wrlock(&g_hlist[hash].lock);
		list_add_tail(&e->list,&g_hlist[hash].list);
		g_hlist_entry++;
		pthread_rwlock_unlock(&g_hlist[hash].lock);

		if (left_len > hdr_len) {
			// ���ж��������
			left_len -= hdr_len;
			p_start += hdr_len;
			p_cur += hdr_len;
			goto again;
		}
	}
	else {
		char *p,*p1,*line_crlf;

again:
		if (e->state != STATE_READ_FILECONTENT1) {
			while ((line_crlf=http_trans_buf_has_patt(p_cur,left_len,"\r\n",2)) != NULL) {
				line_crlf[0]=0;
				switch(e->state) {
					case STATE_READ_DATA:
					case STATE_FILECONTENT_OK:
						if (strncmp(p_cur,e->boundary1+2,strlen(e->boundary1)-2) != 0) {
							break;
						}
						e->state = STATE_BOUNDARY_START;		
						break;
					case STATE_BOUNDARY_START:
						if (strncasecmp(p_cur,"Content-Disposition",19) != 0)
							break;
						if (!(p=strstr(p_cur,"filename="))) {
							// �����ϴ��ļ��Ĳ���
							e->state = STATE_READ_DATA;
							break;
						}
						// test
						//SYSLOG(LOG_NOTICE,"src=%d.%d.%d.%d dst=%d.%d.%d.%d %s",
						//	NIPQUAD(e->src),NIPQUAD(e->dst),p_cur);

						p += 9;
						if (p[0] == '\"') p++;
						p1 = p;
						while (*p1) {	// ǰ���Ѿ���֤��һ���ַ����ˣ�line_crlf[0]=0;��
							if ((*p1)=='\"' || (*p1)==';') {
								*p1 = 0;
								break;
							}
							else if ((*p1)=='/' || (*p1)=='\\')
								p = p1+1;
							p1++;
						}
						if (p[0] == 0) {
							// ����ļ����ǿգ���û�д����ļ�
							e->state = STATE_READ_DATA;
							break;
						}

						// �ļ�����׺���˼��
						if (suffix[0][0] != 0) {
							if ((p1=strrchr(p,'.')) == NULL) {
								e->state = STATE_READ_DATA;
								break;
							}
							p1++;
							for(i=0;i<sizeof(suffix)/sizeof(suffix[0]);i++) {
								if (suffix[i][0] == 0) {
									e->state = STATE_READ_DATA;
									break;
								}
								if (strcmp(suffix[i],p1) == 0)
									break;
							}
							if (e->state == STATE_READ_DATA)
								break;
						}

						// ����ļ����к������ģ����ڵ���������ǲ���utf8�����
						memset(e->cur_filename,0,sizeof(e->cur_filename));
						strncpy(e->cur_filename,p,sizeof(e->cur_filename)-1);
						if (save_dir[0] != 0) {
							memset(e->cur_filepath,0,sizeof(e->cur_filepath));
							snprintf(e->cur_filepath,sizeof(e->cur_filepath)-1,"%s/%d.%d.%d.%d[%d]-%d.%d.%d.%d[%d]%s",
								save_dir,NIPQUAD(e->src),e->sport,NIPQUAD(e->dst),e->dport,p);
							
							P_DEBUG("start open file %s\n",e->cur_filepath);
							e->file_fd = open(e->cur_filepath,O_CREAT|O_RDWR|O_TRUNC,S_IRWXU);
							if (e->file_fd < 0) {
								P_ERR("open file %s failed: %s",e->cur_filepath,strerror(errno));
								e->state = STATE_READ_DATA;
								break;
							}
						}
						e->state = STATE_FILENAME_OK;		
						break;
					case STATE_FILENAME_OK:
						if (line_crlf == p_cur) {
							MD5_Init(&e->md5);
							e->state = STATE_READ_FILECONTENT1;
							if (e->cur_filetype[0] == 0)
								strcpy(e->cur_filetype,"unknown");
						}
						else if (strncasecmp(p_cur,"Content-Type: ",14) == 0) {
							// test
							//SYSLOG(LOG_NOTICE,"src=%d.%d.%d.%d dst=%d.%d.%d.%d %s",
							//	NIPQUAD(e->src),NIPQUAD(e->dst),p_cur);
							//SYSLOG(LOG_NOTICE,"src=%d.%d.%d.%d dst=%d.%d.%d.%d %s",
							//	NIPQUAD(e->src),NIPQUAD(e->dst),e->ua);

							p1 = p_cur+14;
							while(*p1 && *p1 != ' ') p1++;
							p1[0] = 0;
							strncpy(e->cur_filetype,p_cur+14,sizeof(e->cur_filetype)-1);
						}
						break;
					case STATE_BOUNDARY_END:
						break;
					default:
						P_ERR("state is %d\n",e->state);
						break;
				}
				left_len -= (line_crlf+2-p_cur);
				p_cur = line_crlf+2;
				if (e->state == STATE_READ_FILECONTENT1)
					break;
			}
		}

		// ���ﲻ����else�����������жϣ���Ϊ����Ĵ�����ܻ�ı��״̬
		if (e->state == STATE_READ_FILECONTENT1) {
			// read boundary data
			int ret;
			int b_len = strlen(e->boundary1);
			p_start = p_cur;

			while(left_len > 0) {
				if (*p_cur == '\r') {
					if (left_len < b_len+2) {
						// �ַ������������´δչ���˵
						break;
					}
					else if (memcmp(p_cur,e->boundary1,b_len) == 0)  {
						if (p_cur[b_len] == '\r' && p_cur[b_len+1] == '\n') {
							// boundary���������滹��boundary
							if (e->file_fd != -1) {
								ret = io_write(e->file_fd,p_start,p_cur-p_start);
								if (ret < 0) {
									SYSLOG(LOG_ERR,"file %s write error: %s",e->cur_filepath,strerror(errno));
									goto err;
								}						
								close(e->file_fd);
								e->file_fd = -1;
							}
							else
								ret = p_cur-p_start;
							e->cur_len += ret;
							MD5_Update(&e->md5,p_start,p_cur-p_start);
							MD5_Final(e->md5_hash,&e->md5);
							
							time(&t_now);
							tmptr = localtime(&t_now);
							for (i=0;i<16;i++)
								sprintf(e->show_hash+2*i,"%02X",e->md5_hash[i]);
							// ԴIP--Ŀ��IP--ʱ��--�ļ���--URL--�ļ�����--�ļ���С--MD5
							SYSLOG(LOG_NOTICE,"src=%d.%d.%d.%d dst=%d.%d.%d.%d time=%4d-%02d-%02d %02d:%02d:%02d "
								"filename=%s refurl=%s filetype=%s fileszie=%d md5=%-32s",
								NIPQUAD(e->src),NIPQUAD(e->dst),
								tmptr->tm_year+1900,tmptr->tm_mon+1,tmptr->tm_mday,
								tmptr->tm_hour,tmptr->tm_min,tmptr->tm_sec,
								e->cur_filename,e->ref_url,e->cur_filetype,e->cur_len,
								e->show_hash);
							P_BYTE("md5",e->md5_hash,16);
							P_DEBUG("1close file %s\n",e->cur_filepath);

							e->state = STATE_BOUNDARY_START;
							p_cur += b_len+2;
							left_len -= (b_len+2);
							goto again;
						}
						else if (p_cur[b_len] == '-' && p_cur[b_len+1] == '-') {
							// ����boundary����
							if (e->file_fd != -1) {
								ret = io_write(e->file_fd,p_start,p_cur-p_start);
								if (ret < 0) {
									SYSLOG(LOG_ERR,"file %s write error: %s",e->cur_filepath,strerror(errno));
									goto err;
								}
								close(e->file_fd);
								e->file_fd = -1;
							}
							else
								ret = p_cur-p_start;
							e->cur_len += ret;
							MD5_Update(&e->md5,p_start,p_cur-p_start);
							MD5_Final(e->md5_hash,&e->md5);

							time(&t_now);
							tmptr = localtime(&t_now);
							for (i=0;i<16;i++)
								sprintf(e->show_hash+2*i,"%02X",e->md5_hash[i]);
							// ԴIP--Ŀ��IP--ʱ��--�ļ���--URL--�ļ�����--�ļ���С--MD5
							SYSLOG(LOG_NOTICE,"src=%d.%d.%d.%d dst=%d.%d.%d.%d time=%4d-%02d-%02d %02d:%02d:%02d "
								"filename=%s refurl=%s filetype=%s fileszie=%d md5=%-32s",
								NIPQUAD(e->src),NIPQUAD(e->dst),
								tmptr->tm_year+1900,tmptr->tm_mon+1,tmptr->tm_mday,
								tmptr->tm_hour,tmptr->tm_min,tmptr->tm_sec,
								e->cur_filename,e->ref_url,e->cur_filetype,e->cur_len,
								e->show_hash);

							P_BYTE("md5",e->md5_hash,16);
							P_DEBUG("2close file %s\n",e->cur_filepath);
							e->state = STATE_BOUNDARY_END;
							p_cur += b_len+2;
							left_len -= (b_len+2);
							goto again;
						}
					}
				}
				// 
				p_cur++;
				left_len--;
			}
			if (e->file_fd != -1) {
				ret = io_write(e->file_fd,p_start,p_cur-p_start);
				if (ret < 0) {
					SYSLOG(LOG_ERR,"file %s write error: %s",e->cur_filepath,strerror(errno));
					goto err;
				}
			}
			else
				ret = p_cur-p_start;
			e->cur_len += ret;
			MD5_Update(&e->md5,p_start,p_cur-p_start);
		}

		// ��������δ����������ݣ�����Ҫ�������´δ���
		if (left_len > 0) {
			// ���һ�����ݲ��������������һ������
			nids_discard(a_tcp,hlf->count-hlf->offset-left_len);
		}
	}
quit:
	return;

err:
	a_tcp->server.collect--;
	hash = hash_by_tuple4(a_tcp->addr.saddr,a_tcp->addr.daddr,
		a_tcp->addr.source,a_tcp->addr.dest);
	pthread_rwlock_wrlock(&g_hlist[hash].lock);
	list_del(&e->list);
	g_hlist_entry--;
	pthread_rwlock_unlock(&g_hlist[hash].lock);
	conn_ctx_free(e);
}

/*
NIDS�ķ���״̬����
# define NIDS_JUST_EST 1
# define NIDS_DATA 2
# define NIDS_CLOSE 3
# define NIDS_RESET 4
# define NIDS_TIMED_OUT 5
# define NIDS_EXITING   6       // nids is exiting; last chance to get data
*/

void tcp_callback(struct tcp_stream *a_tcp, void ** this_time_not_needed)
{
	if (a_tcp->nids_state == NIDS_JUST_EST) { // ���ӽ���ʱ
		int i;
		for (i=0;i<sizeof(ports)/sizeof(ports[0]);i++) {
			if (ports[i] == 0)
				return;
			if (a_tcp->addr.dest == ports[i])
				break;
		}
		if (i == sizeof(ports)/sizeof(ports[0]))
			return;

		a_tcp->server.collect++; // ֻץclient��server�����ݰ�

		P_DEBUG("%d.%d.%d.%d:%d %d.%d.%d.%d:%d established\n",NIPQUAD(a_tcp->addr.saddr),
			a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);
		return;
	}
	else if (a_tcp->nids_state == NIDS_DATA) { // ���ݴ���ʱ
		struct half_stream *hlf;

		if (a_tcp->server.count_new) {
			hlf = &a_tcp->server;
			process_http(a_tcp);
		}
	}
	else {  // ����������ӹر�ʱ
		int hash;
		struct conn_ctx *e = NULL;

		//printf("a_tcp->nids_state = 0x%x\n",a_tcp->nids_state);
		if (a_tcp->nids_state == NIDS_CLOSE) {
			// connection has been closed normally
			P_DEBUG("%d.%d.%d.%d:%d %d.%d.%d.%d:%d closed\n",NIPQUAD(a_tcp->addr.saddr),
				a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);

			// test
			//SYSLOG(LOG_ERR,"%d.%d.%d.%d:%d %d.%d.%d.%d:%d closed\n",NIPQUAD(a_tcp->addr.saddr),
			//	a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);
		}
		else if (a_tcp->nids_state == NIDS_RESET) {
			// connection has been closed by RST
			P_DEBUG("%d.%d.%d.%d:%d %d.%d.%d.%d:%d reset\n",NIPQUAD(a_tcp->addr.saddr),
				a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);

			//test
			//SYSLOG(LOG_ERR,"%d.%d.%d.%d:%d %d.%d.%d.%d:%d reset\n",NIPQUAD(a_tcp->addr.saddr),
			//	a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);
		}
		else if (a_tcp->nids_state == NIDS_TIMED_OUT) {
			// connection has been timeout
			P_DEBUG("%d.%d.%d.%d:%d %d.%d.%d.%d:%d timeout\n",NIPQUAD(a_tcp->addr.saddr),
				a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);

			//test
			SYSLOG(LOG_ERR,"%d.%d.%d.%d:%d %d.%d.%d.%d:%d timeout\n",NIPQUAD(a_tcp->addr.saddr),
				a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);
		}
		else if (a_tcp->nids_state == NIDS_EXITING) {
			// nids exit, ���������ڶ�ȡץ���ļ�����ʱ��ֱ��ץ����ʱ�������
			P_DEBUG("%d.%d.%d.%d:%d %d.%d.%d.%d:%d exiting\n",NIPQUAD(a_tcp->addr.saddr),
				a_tcp->addr.source,NIPQUAD(a_tcp->addr.daddr),a_tcp->addr.dest);

			// test
			SYSLOG(LOG_ERR,"libnids exiting...");
		}
		else {
			P_DEBUG("a_tcp->nids_state = 0x%x\n",a_tcp->nids_state);

			// test
			SYSLOG(LOG_ERR,"a_tcp->nids_state = 0x%x\n",a_tcp->nids_state);
		}

		e = lookup_by_tuple4(a_tcp->addr.saddr,a_tcp->addr.daddr,
			a_tcp->addr.source,a_tcp->addr.dest);
		if (e) {
			if (e->file_fd != -1) { // �ļ�ץȡû����������ʱ
				time_t t_now;
				struct tm *tmptr;

				time(&t_now);
				tmptr = localtime(&t_now);
				SYSLOG(LOG_ERR,"file capture failed: %d.%d.%d.%d %d.%d.%d.%d %4d-%02d-%02d %02d:%02d:%02d %s %s %s",
					NIPQUAD(e->src),NIPQUAD(e->dst),
					tmptr->tm_year+1900,tmptr->tm_mon+1,tmptr->tm_mday,
					tmptr->tm_hour,tmptr->tm_min,tmptr->tm_sec,
					e->cur_filename,e->post_url,e->cur_filetype);
			}

			hash = hash_by_tuple4(a_tcp->addr.saddr,a_tcp->addr.daddr,
				a_tcp->addr.source,a_tcp->addr.dest);
			pthread_rwlock_wrlock(&g_hlist[hash].lock);
			list_del(&e->list);
			g_hlist_entry--;
			pthread_rwlock_unlock(&g_hlist[hash].lock);
			conn_ctx_free(e);
		}
	}

	return;
}

void usage(void)
{
	printf("usage: httpsniff {OPTIONS}\n"
		"OPTIONS:\n"
		"    -i ethx:  NIC interface name, such as eth0, eth1 or eth2, default [eth0]\n"
		"    -d level: debug level, 0 1 2 ... 8, default [0] means no debug\n"
		"    -p ports: TCP port to capture, could be multiple(max 16), default [80]\n"
		"    -f suffix: File suffix filter, could be multiple(max 32), \n"
		"       default [doc docx xls xlsx ppt pptx wps pdf html htm txt ceb zip rar]\n"
		"       if suffix is set to \"null\", it means unlimited\n"
		"    -x size: min file size[byte], default 0, which means unlimited\n"
		"    -y size: max file size[byte], default 0, which means unlimited\n"
		"    -w dir:   dir to save upload files, default [upload]\n"
		"       if dir is set to \"null\", it means disable it\n"
		"    -v: show version\n"
		"    -h: show this\n"
		"NOTE:\n"
		"    to show programme status, do \"kill -SIGHUP `cat /var/run/httpsniff.pid`\",\n"
		"    and then \"cat /tmp/httpsniff_status.txt\"\n");
} 

char buffer[64<<10];
void show_detail(void)
{
	int i,j,n,len;
	time_t t;
	struct conn_ctx *e;
	struct list_head *pos;
	char show_buffer[256] = {0};
	FILE *fp = NULL;

	time(&t);
	n = 0;
	j = 1;
	len = snprintf(buffer,sizeof(buffer),"Hash table:\n");
	for(i=0;i<HASHSIZE && j>0;i++) {
		pthread_rwlock_rdlock(&g_hlist[i].lock);
		if (list_empty(&g_hlist[i].list)) {
			pthread_rwlock_unlock(&g_hlist[i].lock);
			continue;
		}
		len += snprintf(buffer+len,sizeof(buffer)-len,"table %d:\n",i);
		list_for_each(pos,&g_hlist[i].list) {
			e = list_entry(pos,struct conn_ctx,list);
			len += snprintf(buffer+len,sizeof(buffer)-len,
				"%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d %ds\n",NIPQUAD(e->src),e->sport,
				NIPQUAD(e->dst),e->dport,(int)t-e->start_time);
			n++;
			if (len > (sizeof(buffer)-1024)) {
				len += snprintf(buffer+len,sizeof(buffer)-len,"......left more ......\n");
				j = 0;
				break;
			}
		}
		pthread_rwlock_unlock(&g_hlist[i].lock);
	}
	len += snprintf(buffer+len,sizeof(buffer)-len,"Show elements: %d\n",n);
	len += snprintf(buffer+len,sizeof(buffer)-len,"Total elements: %d\n",g_hlist_entry);

	n = 0;
	for (i=0;i<sizeof(ports)/sizeof(ports[0]);i++) {
		if (ports[i] != 0)
			n += snprintf(show_buffer+n,sizeof(show_buffer)-1-n,"%d ",ports[i]);
	}

	len += snprintf(buffer+len,sizeof(buffer)-len,"Capture tcp ports %son %s\n",
		show_buffer,nids_params.device);

	len += snprintf(buffer+len,sizeof(buffer)-len,"Debug level: %d, save dir: %s\n",
		debug,(save_dir[0]==0)?"disable":save_dir);

	if (filesize_min == 0)
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file length min: unlimited\n");
	else
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file length min: %d bytes\n",filesize_min);

	if (filesize_max == 0)
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file length max: unlimited\n");
	else
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file length max: %d bytes\n",filesize_max);

	n = 0;
	for (i=0;i<sizeof(suffix)/sizeof(suffix[0]);i++) {
		if (suffix[i][0] == 0)
			break;
		n += snprintf(show_buffer+n,sizeof(show_buffer)-1-n,"%s ",suffix[i]);
	}
	if (n == 0)
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file suffix: unlimited\n");
	else
		len += snprintf(buffer+len,sizeof(buffer)-len,"Capture file suffix: %s\n",show_buffer);

	if (NULL==(fp=fopen("/tmp/httpsniff_status.txt","wb"))) {
		P_ERR("fopen failed: %s",strerror(errno));
		return;
	}
	fwrite(buffer,1,len,fp);
	fclose(fp);

	return;
}

/* handle some signals */
static void handle_signal(int sig)
{
	if(sig == SIGCHLD) {
		pid_t pid;
		int stat;

		while((pid = waitpid(-1, &stat, WNOHANG)) > 0){
			P_DEBUG("child %d terminated\n", pid);
		}
		return;
	}
	if (sig == SIGHUP) {
		show_detail();
		return;
	}
	P_DEBUG("rcv signal %d\n",sig);
	SYSLOG(LOG_WARNING,"warn:rcv signal %d\n",sig);
	SYSLOG(LOG_ALERT,"alert:programme exit\n");

	unlink(PIDFILE);
	exit(1);
}


int main (int argc, char *argv[])
{
	int i = 0;
	int opt,len=0;
	char *p1,*p2;
	char ethx[16] = "eth0";
	char show_ports[256] = {0};
	struct stat s1;
	FILE *fp = NULL;
	struct nids_chksum_ctl chksumctl;

	while ((opt = getopt(argc, argv, "i:d:p:f:x:y:w:vh")) != -1) {
		switch(opt) {
		case 'i':
			strncpy(ethx,optarg,sizeof(ethx)-1);
			break;
		case 'd':
			debug = atoi(optarg)%8;
			break;
		case 'p':
			i = 0;
			ports[i++] = atoi(optarg);
			while (optind < argc && argv[optind][0] != '-' 
				&& i < sizeof(ports)/sizeof(ports[0])) {
				ports[i++] = atoi(argv[optind]);
				optind++;
			}
			break;
		case 'f':
			i = 0;
			p1 = optarg;
			p2 = optarg;
			while (i<sizeof(suffix)/sizeof(suffix[0])) {
				if (*p2 == ',') {
					*p2 = 0;
					strncpy(suffix[i],p1,sizeof(suffix[0])-1);
					i++;
					p2++;
					p1 = p2;
				}
				else if (*p2 == 0) {
					strncpy(suffix[i],p1,sizeof(suffix[0])-1);
					i++;
					break;
				}
				else
					p2++;
			}
			if (i < sizeof(suffix)/sizeof(suffix[0]))
				suffix[i][0] = 0;
			break;
		case 'x':
			filesize_min = atoi(optarg);
			break;
		case 'y':
			filesize_max = atoi(optarg);
			break;
		case 'w':
			strncpy(save_dir,optarg,sizeof(save_dir)-1);
			if (strcmp(save_dir,"null") == 0)
				save_dir[0] = 0; // disable save file
			break;
		case 'v':
			printf("%s, build %s\n",VERSION,BUILDTIME);
			return 0;
		case 'h':
		default:
			usage();
			return -1;
		}
	}
	
	if (stat(PIDFILE,&s1) != -1) {
		// already running
		printf("The programme pid file(%s) is already exist, quit\n",PIDFILE);
		return -1;
	}
	if (NULL==(fp=fopen(PIDFILE,"wb"))) {
		printf("create pid file failed: %s\n",strerror(errno));
		return -1;
	}
	fprintf(fp,"%u",getpid());
	fclose(fp);

	signal(SIGINT,handle_signal);
	signal(SIGTERM,handle_signal);
	signal(SIGSEGV,handle_signal);
	signal(SIGPIPE,SIG_IGN);
	signal(SIGCHLD,handle_signal);
	signal(SIGHUP,handle_signal);

	for (i=0;i<HASHSIZE;i++) {
		INIT_LIST_HEAD(&g_hlist[i].list);
		pthread_rwlock_init(&g_hlist[i].lock,NULL);
	}
	chksumctl.netaddr = inet_addr("0.0.0.0");
	chksumctl.mask = inet_addr("0.0.0.0");
	chksumctl.action = NIDS_DONT_CHKSUM;

	// set nids_param
	nids_params.device = ethx;
	nids_params.scan_num_hosts = 0; // ����Ҫ�˿�ɨ�蹦��

	// TCP���ӵ�����������libnidsȡn_tcp_streamsֵ��3/4��Ϊ���TCP��������������Ĭ��1040��
	// ����ʱ����������û�л�������TCP���ӣ�����NIDS_TIME_OUT״̬����������־�澯
	nids_params.n_tcp_streams=10400;

	// IP��Ƭ����hash table��С��Ĭ��256
	//nids_params.n_hosts=256;

	// ������ʾ��1���߳�ͨ��libpcapץ������һ���̴߳���ץ���İ��������������Լ������ص�������
	// Ĭ�Ϲرգ���ʾ��ֻ��һ���߳̽���ץ�����������ݴ���
	nids_params.multiproc=1;

	// nids_params.multiproc=1ʱ�������壬��ʾץ�����е��������Ĭ��20000��
	//��������ʱ��ᶪ�������İ�����������־�澯
	nids_params.queue_limit=500000; 

	// TCP���ӳ�ʱ��=1������Ĭ�Ϲر�
	// ������ĳ�ʱ���������TCP�رչ��̵ĳ�ʱ�����Ѿ���⵽��һ��FIN����ʼ���٣�
	// �����һ����ʱ���ڣ�10s��û�н��յ�������TCP���ӹرյĹ��̣��򷵻�NIDS_TIME_OUT״̬��
	// PS: ���û��ץ���رչ��̵����ݰ���һ����ûץ�������������libnids��Ȼ��Ϊ��TCP������������ô����?
	// ����Ҳûɶ��ϵ����Ϊlibnidsͬʱ���ٵ�TCP�������и����ޣ������ã���ǰ��Ĳ�����������������ޣ�
	// ���û�л�������TCP���Ӷ���������NIDS_TIME_OUT״̬�������������TCP��������Ȼ��Ϊû�л����������ӡ�
	// ֻ�����ͬʱ���ٵ�TCP������û�е�������ʱ�����������TCP���ӾͿ���һֱ������...�����Ҫ������������Ļ���
	// ��Ҫ�������Լ������ˣ������TCP�Ļص�ע�ắ���ﴦ��������libnids�¼�������ͻ
	//nids_params.tcp_workarounds=1;

	// �����Ƿ���Ҫ����pcap_filter���Ա���ٶԲ���ɵİ���ץȡ�أ�
	// nids.pcap_filter = xxx;

	for (i=0;i<sizeof(ports)/sizeof(ports[0]);i++) {
		if (ports[i] != 0)
			len += snprintf(show_ports+len,sizeof(show_ports)-1-len,"%d ",ports[i]);
	}
	if (save_dir[0] == 0) {
		printf("start capture tcp port %son %s\n",show_ports,nids_params.device);
		SYSLOG(LOG_INFO,"start capture tcp port %son %s\n",show_ports,nids_params.device);
	}
	else {
		printf("start capture tcp port %son %s, save file to dir %s\n",show_ports,
			nids_params.device,save_dir);
		SYSLOG(LOG_INFO,"start capture tcp port %son %s, save file to dir %s\n",show_ports,
			nids_params.device,save_dir);
	}
	if (!nids_init ()) {
		printf("%s\n",nids_errbuf);
		unlink(PIDFILE);
		exit(1);
	}
	// nids_init���openlog�������������Ļص������������Ұ�ident��Ϊlibnids������ĳ����ǵĳ�����
	closelog();
	openlog("httpsniff",0,LOG_LOCAL0);

	nids_register_tcp(tcp_callback);
	nids_register_chksum_ctl(&chksumctl,1);
	nids_run();

	return 0;
}

