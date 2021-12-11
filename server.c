#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#define OPEN_MAX 1024

int gfd,lfd;


void send_error(int cfd, int op, char *title, char *text);
const char *get_file_type(const char *name);
void initepoll();
int get_line(int cfd, char *buf, int len);
void disconnect(int fd);
void initsocket(int port, char *dir);
void acceptrun();
void sys_err(char *err);
void encode_str(char* to, int tosize, const char* from);
void decode_str(char *to, char *from);
int hexit(char c);

void send_dir(int cfd, char *file)
{
	int i, ret;
	char buf[4096] = {0};

	sprintf(buf, "<html><head><title>directory: %s</title></head>", file);
	sprintf(buf+strlen(buf),"<body><h1>cur dir: %s</h1><table>",file);

	char enstr[1024];
	char path[1024];

	struct dirent **ptr;

	int num = scandir(file, &ptr, NULL, alphasort);

	for(i = 0; i < num; i++)
	{
		char *name = ptr[i]->d_name;
		sprintf(path, "%s%s",file, name);
		printf("path: %s\n",path);
		struct stat st;
		stat(path,&st);

		encode_str(enstr, sizeof(enstr), name);

        if(S_ISREG(st.st_mode)) {       
            sprintf(buf+strlen(buf), 
                    "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                    enstr, name, (long)st.st_size);
        } else if(S_ISDIR(st.st_mode)) {		// 如果是目录       
            sprintf(buf+strlen(buf), 
                    "<tr><td><a href=\"%s/\">%s/</a></td><td>%ld</td></tr>",
                    enstr, name, (long)st.st_size);
        }
        ret = send(cfd, buf, strlen(buf), 0);
        if (ret == -1) {
            if (errno == EAGAIN) {
                perror("send error:");
                continue;
            } else if (errno == EINTR) {
                perror("send error:");
                continue;
            } else {
                perror("send error:");
                exit(1);
            }
        }
        memset(buf, 0, sizeof(buf));
        // 字符串拼接
    }

    sprintf(buf+strlen(buf), "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);

    printf("dir message send OK!!!!\n");

}


void send_file(int cfd, char *file)
{
	int fd = open(file, O_RDONLY);
	int n;
	char buf[4096];

	if(fd == -1)
	{
		perror("open");
		send_error(cfd, 404, "Not Found", "No such file or directory");
		return;
	}

	while((n = read(fd, buf, sizeof(buf))) > 0)
	{
		int ret = send(cfd, buf, n, 0);
		if(ret == -1)
		{
			if(errno == EINTR || errno == EAGAIN)
			{
				continue;
			}
			else
			{
				perror("send error");
				send_error(cfd, 404, "Not Found", "No such file or directory");
			}
		}

	}
	if(n == -1)
	{
		perror("read error");
		send_error(cfd, 404, "Not Found", "No such file or directory");
	}
	close(fd);
}

void send_respond_head(int cfd, int op, char *title, const char *text, long len)
{
	printf("%s: %s\n",__FUNCTION__,text);
	char buf[1024] = {0};

	sprintf(buf, "HTTP/1.1 %d %s\r\n", op, title);
	send(cfd, buf, strlen(buf), 0);

	sprintf(buf, "Content-Type:%s\r\n",text);
	sprintf(buf+strlen(buf), "Content-Length:%ld\r\n",len);
	send(cfd, buf, strlen(buf), 0);

	send(cfd, "\r\n", 2, 0);

}

void http_request(char *str, int cfd)
{
	char head[16], path[256], protocol[16];

	sscanf(str, "%[^ ] %[^ ] %[^ ]", head, path, protocol);
	printf("%s %s %s\n",head, path, protocol);

	char *file = path + 1;

	if(strcmp(path, "/") == 0)
	{
		file = "./";
	}

	struct stat st;
	int ret = stat(file, &st);	
	printf("%s: %s\n",__FUNCTION__, file);
	if(ret == -1)
	{
		send_error(cfd, 404, "Not Found", "No such file or directory");
		return;
	}
	if(S_ISDIR(st.st_mode))
	{
		send_respond_head(cfd, 200, "OK",get_file_type(".html"), -1);

		send_dir(cfd,file);
	}
	else if(S_ISREG(st.st_mode))
	{
		send_respond_head(cfd, 200, "OK", get_file_type(file), -1);
		send_file(cfd, file);
	}


}

void read_data(int fd)
{
	int n;
	char line[1024] = {0};
	n = get_line(fd, line, sizeof(line));
	if(n == -1)
	{
		if(errno == EINTR)
		{
		}
		printf("----------read failed\n");
		exit(1);
	}
	else if(n == 0)
	{
		disconnect(fd);
	}
	else{
		while(1)
		{
			char buf[1024] = {0};
			n = get_line(fd, buf, sizeof(buf));
			if(buf[0] == '\n')
			{
				break;
			}
			else if(n == -1 || n == 0)
			{
				break;
			}
		}
	}

	if(strncasecmp("get", line, 3) == 0)
	{
		http_request(line, fd);

		disconnect(fd);
	}


}

void epollrun()
{
	struct epoll_event evts[OPEN_MAX];
	int nfds;

	while(1)
	{
		nfds = epoll_wait(gfd,evts, OPEN_MAX, -1);
		if(nfds == -1)
			sys_err("epoll_wait");

		for(int i = 0; i < nfds; i++)
		{
			if(!(evts[i].events & EPOLLIN))
			{
				continue;
			}

			if(evts[i].data.fd == lfd)
			{
				acceptrun();
			}
			else
			{
				read_data(evts[i].data.fd);
			}
		}

	}

}

int main(int argc, char *argv[])
{
	if(argc < 3)
	{
		printf("please enter port and workdir\n");
		exit(1);
	}

	int port = atoi(argv[1]);

	initsocket(port,argv[2]);

	initepoll();

	epollrun();


	return 0;
}

void initepoll()
{
	struct epoll_event ev;
	
	ev.events = EPOLLIN;
	ev.data.fd = lfd;

	gfd = epoll_create(OPEN_MAX);

	epoll_ctl(gfd, EPOLL_CTL_ADD, lfd, &ev);
}

void initsocket(int port, char *dir)
{
	chdir(dir);

	printf("%s: %s\n",__FUNCTION__, dir);
	struct sockaddr_in sev;
	sev.sin_family = AF_INET;
	sev.sin_port = htons(port);
	sev.sin_addr.s_addr = htonl(INADDR_ANY);

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if(lfd == -1)
		sys_err("socket");

	int flag = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

	bind(lfd, (struct sockaddr*)&sev, sizeof(sev));
	if(lfd == -1)
		sys_err("bind");

	listen(lfd, 128);
	if(lfd == -1)
		sys_err("listen");
}

void sys_err(char *err)
{
	perror(err);
	exit(1);
}

void disconnect(int fd)
{
	epoll_ctl(gfd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
	printf("has disconnected\n");

}

void send_error(int cfd, int op, char *title, char *text)
{
	char buf[4096] = {0};

	sprintf(buf, "%s %d %s\r\n","HTTP/1.1", op, title);
	sprintf(buf+strlen(buf),"Content-Type:%s\r\n","text/html");
	sprintf(buf+strlen(buf),"Content-Length:%d\r\n",-1);
	sprintf(buf+strlen(buf),"Connection: close\r\n");
	send(cfd, buf, strlen(buf), 0);
	send(cfd, "\r\n", 2, 0);

	memset(buf, 0, sizeof(buf));

	sprintf(buf, "<html><head><title>%d %s</title></head>\n",op , title);
	sprintf(buf+strlen(buf), "<body bgcolor=\"#cc99cc\"><h2 align=\"center\">%d %s</h4>\n", op, title);
	sprintf(buf+strlen(buf), "%s\n", text);
	sprintf(buf+strlen(buf), "<hr>\n</body>\n</html>\n");
	send(cfd, buf, strlen(buf), 0);
	
}

void acceptrun()
{
	struct sockaddr_in cli;
	memset(&cli, 0, sizeof(cli));
	socklen_t clilen = sizeof(cli);
	int cfd;

again:
	cfd = accept(lfd, (struct sockaddr*)&cli,&clilen);
	if(cfd == -1)
	{
		if(errno == EINTR || errno == ECONNABORTED)
		{
			goto again;
		}
		sys_err("accept");
	}
	fcntl(cfd, F_SETFL, O_NONBLOCK);

	struct epoll_event ev;
	ev.events = EPOLLIN |EPOLLET;
	ev.data.fd = cfd;

	int ret = epoll_ctl(gfd, EPOLL_CTL_ADD, cfd, &ev);
	if(ret == -1)
		sys_err("client: epoll_ctl");

}

int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;
    while ((i < size - 1) && (c != '\n')) {    
        n = recv(sock, &c, 1, 0);
        if (n > 0) {        
            if (c == '\r') {            
                n = recv(sock, &c, 1, MSG_PEEK);
                if ((n > 0) && (c == '\n')) {               
                    recv(sock, &c, 1, 0);
                } else {                            
                    c = '\n';
                }
            }
            buf[i] = c;
            i++;
        } else {       
            c = '\n';
        }
    }
    buf[i] = '\0';

    return i;
}
void encode_str(char* to, int tosize, const char* from)
{
    int tolen;

    for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {    
        if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0) {      
            *to = *from;
            ++to;
            ++tolen;
        } else {
            sprintf(to, "%%%02x", (int) *from & 0xff);
            to += 3;
            tolen += 3;
        }
    }
    *to = '\0';
}

void decode_str(char *to, char *from)
{
    for ( ; *from != '\0'; ++to, ++from  ) {     
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {       
            *to = hexit(from[1])*16 + hexit(from[2]);
            from += 2;                      
        } else {
            *to = *from;
        }
    }
    *to = '\0';
}

int hexit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

const char *get_file_type(const char *name)
{
    char* dot;

    // 自右向左查找‘.’字符, 如不存在返回NULL
    dot = strrchr(name, '.');   
    if (dot == NULL)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp( dot, ".wav" ) == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}
