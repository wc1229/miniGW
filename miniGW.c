#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/rtc.h>
#include <net/if.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>


#define RTC_DEVICE_NAME "/dev/rtc0"

static pthread_mutex_t mutex;
static FILE *fp = NULL;
static struct rtc_time rtc;
#define UDP_RECV_BUFFER_SIZE	2048
#define CAN_RECV_BUFFER_SIZE	8

static void gw_log(char *error)
{
	pthread_mutex_lock(&mutex); //互斥锁上锁
    int fd = open(RTC_DEVICE_NAME, O_RDWR);
	fp = fopen("/home/root/log.txt", "a");
    ioctl(fd,RTC_RD_TIME,&rtc);
	fprintf(fp, "时间: %d-%d-%d %d:%d:%d\n",
            rtc.tm_year+1900,
            rtc.tm_mon+1,
            rtc.tm_mday,
            rtc.tm_hour,
            rtc.tm_min,
            rtc.tm_sec);
	fprintf(fp, "%s\n", error);
	fclose(fp);
	pthread_mutex_unlock(&mutex);//互斥锁解锁
}

/*增加udp信息，倒插法，“\”是为了将信息连成一行*/
#define LL_ADD(item, list) do {		\
	item->prev = NULL;				\
	item->next = list;				\
	if (list != NULL) list->prev = item; \
	list = item;					\
} while(0)  //为了将这个宏定义当成函数来用，也就是说为了匹配“;”，所以采用while(0)

/*删除udp信息*/
#define LL_REMOVE(item, list) do {		\
	if (item->prev != NULL) item->prev->next = item->next;	\
	if (item->next != NULL) item->next->prev = item->prev;	\
	if (list == item) list = item->next;	\
	item->prev = item->next = NULL;			\
} while(0)

/***************************************************************
udp数据区
 ***************************************************************/

/*udp结构体*/
struct udp_entry {

	char buffer[UDP_RECV_BUFFER_SIZE];
	
	/*双向对齐*/
	struct udp_entry *next;
	struct udp_entry *prev;
	
};

/*udp表*/
struct udp_table {

	struct udp_entry *entries;
	int count;

	pthread_spinlock_t spinlock;
};



static struct  udp_table *udpt = NULL;

/*单例模式*/
static struct  udp_table *udp_table_instance(void) {

	if (udpt == NULL) {

		udpt = malloc(sizeof(struct  udp_table));
		if (udpt == NULL) {
			gw_log("udp_table_instance error");
			exit(EXIT_FAILURE);
		}
		memset(udpt, 0, sizeof(struct  udp_table));

		pthread_spin_init(&udpt->spinlock, PTHREAD_PROCESS_SHARED);
	}

	return udpt;

}

/*将数据插入udp表*/
static int ng_insert_udp(size_t len, char *buffer) {

	struct udp_table *table = udp_table_instance();

	struct udp_entry *entry = calloc(1,sizeof(struct udp_entry));

	if (entry) {
		memcpy(entry->buffer, buffer, len);

		pthread_spin_lock(&table->spinlock);
		LL_ADD(entry, table->entries);
		table->count ++;
		pthread_spin_unlock(&table->spinlock);
		
	    return 1; //添加成功
	}

	return 0; //添加失败
}

/*将数据从udp表取出*/
static int ng_get_udp(char* buffer) {

	struct udp_entry *iter;
	struct udp_table *table = udp_table_instance();
	if(table->count<=0)return 0;

	pthread_spin_lock(&table->spinlock);
	iter = table->entries;
	memcpy(buffer, iter->buffer, UDP_RECV_BUFFER_SIZE);
	LL_REMOVE(iter, table->entries);
	table->count --;
	pthread_spin_unlock(&table->spinlock);

	free(iter);

	return 1;
}


/***************************************************************
can数据区
 ***************************************************************/

/*can结构体*/
struct can_entry {

	char buffer[CAN_RECV_BUFFER_SIZE];
	
	/*双向对齐*/
	struct can_entry *next;
	struct can_entry *prev;
	
};

/*can表*/
struct can_table {

	struct can_entry *entries;
	int count;

	pthread_spinlock_t spinlock;
};



static struct  can_table *cant = NULL;

/*单例模式*/
static struct  can_table *can_table_instance(void) {

	if (cant == NULL) {

		cant = malloc(sizeof(struct  can_table));
		if (cant == NULL) {
			gw_log("can_table_instance error");
			exit(EXIT_FAILURE);
		}
		memset(cant, 0, sizeof(struct  can_table));

		pthread_spin_init(&cant->spinlock, PTHREAD_PROCESS_SHARED);
	}

	return cant;

}

/*将数据插入can表*/
static int ng_insert_can(__u8 len, __u8 *buffer) {

	struct can_table *table = can_table_instance();

	struct can_entry *entry = calloc(1,sizeof(struct can_entry));

	if (entry) {
		memcpy(entry->buffer, (char*)buffer, (size_t)len);

		pthread_spin_lock(&table->spinlock);
		LL_ADD(entry, table->entries);
		table->count ++;
		pthread_spin_unlock(&table->spinlock);
		
	    return 1; //添加成功
	}

	return 0; //添加失败
}

/*将数据从can表取出*/
static int ng_get_can(char* buffer) {

	struct can_entry *iter;
	struct can_table *table = can_table_instance();
	if(table->count <= 0)return 0;

	pthread_spin_lock(&table->spinlock);
	iter = table->entries;
	memcpy(buffer, iter->buffer, CAN_RECV_BUFFER_SIZE);
	LL_REMOVE(iter, table->entries);
	table->count --;
	pthread_spin_unlock(&table->spinlock);

	free(iter);

	return 1;
}


static void *udp0_read(void *arg)
{
    int ret;

	int connfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (0 > connfd) {
		gw_log("udp0_read socket error");
        perror("udp0_read socket error");
        close(connfd);
        exit(EXIT_FAILURE);
    }

	struct sockaddr_in localaddr, clientaddr; // struct sockaddr
	memset(&localaddr, 0, sizeof(struct sockaddr_in));

	localaddr.sin_port = htons(8888);
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = inet_addr("192.168.222.62"); // 0.0.0.0INADDR_ANY
	

	ret = bind(connfd, (struct sockaddr*)&localaddr, sizeof(localaddr));
    if (0 > ret) {
		gw_log("udp0_read bind error");
        perror("udp0_read bind error");
        close(connfd);
        exit(EXIT_FAILURE);
    }

	char buffer[UDP_RECV_BUFFER_SIZE] = {0};
	socklen_t addrlen = sizeof(clientaddr);
	while (1) {
		ret = recvfrom(connfd, buffer, UDP_RECV_BUFFER_SIZE, 0, 
			(struct sockaddr*)&clientaddr, &addrlen);
		if(0 >= ret) {
		    gw_log("udp0_read recvfrom error");
            perror("udp0_read recvfrom error");
            close(connfd);
            break;
        }
        ret = ng_insert_udp(strlen(buffer), buffer);
		if(0 == ret){
		    gw_log("ng_insert_udp error");
            perror("ng_insert_udp error");
            close(connfd);
            break;
		}
		memset(buffer, 0, UDP_RECV_BUFFER_SIZE);
	}

	close(connfd);
	exit(EXIT_SUCCESS);
}

static void *udp0_write(void *arg)
{
    int ret;

	int connfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (0 > connfd) {
		gw_log("udp0_write socket error");
        perror("udp0_write socket error");
        close(connfd);
        exit(EXIT_FAILURE);
    }

	struct sockaddr_in localaddr, clientaddr; // struct sockaddr
	memset(&localaddr, 0, sizeof(struct sockaddr_in));

	localaddr.sin_port = htons(8889);
	localaddr.sin_family = AF_INET;
	localaddr.sin_addr.s_addr = inet_addr("192.168.222.62"); // 0.0.0.0INADDR_ANY
	
	clientaddr.sin_port = htons(8889);
	clientaddr.sin_family = AF_INET;
	clientaddr.sin_addr.s_addr = inet_addr("192.168.222.102"); // 0.0.0.0INADDR_ANY
	

	ret = bind(connfd, (struct sockaddr*)&localaddr, sizeof(localaddr));
    if (0 > ret) {
		gw_log("udp0_write bind error");
        perror("udp0_write bind error");
        close(connfd);
        exit(EXIT_FAILURE);
    }

	char *buffer = (char*)calloc(UDP_RECV_BUFFER_SIZE, sizeof(char));
	char *tmp = NULL;
	while (1) {
		memset(buffer, 0, UDP_RECV_BUFFER_SIZE);
		tmp = buffer;
		for(int i=0;i<128;i++){//填充1024（8*128）个字节的数据）
			ret = ng_get_can(tmp);
			if(1 == ret){
				tmp += 8;
			}
			else break;
		}
		// tmp = '\0';
		// printf("recv from %s:%d, data:%s\n", inet_ntoa(clientaddr.sin_addr), 
		// 	ntohs(clientaddr.sin_port), buffer);
		if(buffer != tmp)ret = sendto(connfd, buffer, (size_t)(tmp-buffer), 0, 
			(struct sockaddr*)&clientaddr, sizeof(clientaddr));
		if(0 > ret) {
		    gw_log("udp0_write sendto error");
            perror("udp0_write sendto error");
            close(connfd);
            break;
        }
		sleep(1);
	}

	close(connfd);
	exit(EXIT_SUCCESS);
}

static void *can0_read(void *arg)
{
	struct ifreq ifr = {0};
	struct sockaddr_can can_addr = {0};
	struct can_frame frame = {0};
	int sockfd = -1;
	int ret;
    struct can_filter rfilter[1];
	rfilter[0].can_id = 0x00000010;
	rfilter[0].can_mask  = 0x1FFFFFFF;

	/* 打开套接字 */
	sockfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if(0 > sockfd) {
		gw_log("can0_read socket error");
		perror("socket error");
		exit(EXIT_FAILURE);
	}

	/* 指定can0设备 */
	strcpy(ifr.ifr_name, "can0");
	ioctl(sockfd, SIOCGIFINDEX, &ifr);
	can_addr.can_family = AF_CAN;
	can_addr.can_ifindex = ifr.ifr_ifindex;

	/* 将can0与套接字进行绑定 */
	ret = bind(sockfd, (struct sockaddr *)&can_addr, sizeof(can_addr));
	if (0 > ret) {
		gw_log("can0_read bind error");
		perror("bind error");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	/* 设置过滤规则 */
	//setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);
	setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, &rfilter, sizeof(rfilter));

	/* 接收数据 */
	for ( ; ; ) {
		if (0 > read(sockfd, &frame, sizeof(struct can_frame))) {
		    gw_log("can0_read read error");
			perror("read error");
			break;
		}

		/* 校验是否接收到错误帧 */
		if (frame.can_id & CAN_ERR_FLAG) {
			printf("Error frame!\n");
			break;
		}

		/* 校验帧格式 */
		if (frame.can_id & CAN_EFF_FLAG)continue;

		/* 校验帧类型：数据帧还是远程帧 */
		if (frame.can_id & CAN_RTR_FLAG) {
			printf("remote request\n");
			continue;
		}

        ret = ng_insert_can(frame.can_dlc, frame.data);
		if(0 == ret){
		    gw_log("ng_insert_can error");
            perror("ng_insert_can error");
            close(sockfd);
            break;
		}

		// /* 打印数据长度 */
		// printf("[%d] ", frame.can_dlc);

		// /* 打印数据 */
		// for (i = 0; i < frame.can_dlc; i++)
		// 	printf("%02x ", frame.data[i]);
		// printf("\n");

	}

	/* 关闭套接字 */
	close(sockfd);
	exit(EXIT_SUCCESS);
}

static void *can1_read(void *arg)
{
	struct ifreq ifr = {0};
	struct sockaddr_can can_addr = {0};
	struct can_frame frame = {0};
	int sockfd = -1;
	int ret;
    struct can_filter rfilter[1];
	rfilter[0].can_id = 0x00000011;
	rfilter[0].can_mask  = 0x1FFFFFFF;

	/* 打开套接字 */
	sockfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if(0 > sockfd) {
		gw_log("can1_read socket error");
		perror("socket error");
		exit(EXIT_FAILURE);
	}

	/* 指定can1设备 */
	strcpy(ifr.ifr_name, "can1");
	ioctl(sockfd, SIOCGIFINDEX, &ifr);
	can_addr.can_family = AF_CAN;
	can_addr.can_ifindex = ifr.ifr_ifindex;

	/* 将can0与套接字进行绑定 */
	ret = bind(sockfd, (struct sockaddr *)&can_addr, sizeof(can_addr));
	if (0 > ret) {
		gw_log("can1_read bind error");
		perror("bind error");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	/* 设置过滤规则 */
	//setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);
	setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, &rfilter, sizeof(rfilter));

	/* 接收数据 */
	for ( ; ; ) {
		if (0 > read(sockfd, &frame, sizeof(struct can_frame))) {
		    gw_log("can1_read read error");
			perror("read error");
			break;
		}

		/* 校验是否接收到错误帧 */
		if (frame.can_id & CAN_ERR_FLAG) {
			printf("Error frame!\n");
			break;
		}

		/* 校验帧格式 */
		if (frame.can_id & CAN_EFF_FLAG)continue;

		/* 校验帧类型：数据帧还是远程帧 */
		if (frame.can_id & CAN_RTR_FLAG) {
			printf("remote request\n");
			continue;
		}

        ret = ng_insert_can(frame.can_dlc, frame.data);
		if(0 == ret){
		    gw_log("ng_insert_can error");
            perror("ng_insert_can error");
            close(sockfd);
            break;
		}

		// /* 打印数据长度 */
		// printf("[%d] ", frame.can_dlc);

		// /* 打印数据 */
		// for (i = 0; i < frame.can_dlc; i++)
		// 	printf("%02x ", frame.data[i]);
		// printf("\n");
	}

	/* 关闭套接字 */
	close(sockfd);
	exit(EXIT_SUCCESS);
}

static void *can0_write(void *arg)
{
	struct ifreq ifr = {0};
	struct sockaddr_can can_addr = {0};
	struct can_frame frame = {0};
	int sockfd = -1;
	int ret;

	/* 打开套接字 */
	sockfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if(0 > sockfd) {
		gw_log("can0_write socket error");
		perror("socket error");
		exit(EXIT_FAILURE);
	}

	/* 指定can0设备 */
	strcpy(ifr.ifr_name, "can0");
	ioctl(sockfd, SIOCGIFINDEX, &ifr);
	can_addr.can_family = AF_CAN;
	can_addr.can_ifindex = ifr.ifr_ifindex;

	/* 将can0与套接字进行绑定 */
	ret = bind(sockfd, (struct sockaddr *)&can_addr, sizeof(can_addr));
	if (0 > ret) {
		gw_log("can0_write bind error");
		perror("bind error");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	/* 设置过滤规则：不接受任何报文、仅发送数据 */
	setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);

	frame.can_dlc = 8;	//一次发送6个字节数据
	frame.can_id = 0x111;//帧ID为0x123,标准帧

	char *buffer = (char*)calloc(UDP_RECV_BUFFER_SIZE, sizeof(char));
    char *tmp = NULL;
	while(1) {
		memset(buffer, 0, UDP_RECV_BUFFER_SIZE);
		ret = ng_get_udp(buffer);
		if(0 == ret)continue;
		tmp = buffer;
		int n = (int)(strlen(buffer)/8);

		for(int i=0;i<n;i++){//1024（8*128）个字节的数据）
			/* 发送数据 */
		    memcpy((char*)frame.data, tmp, (size_t)frame.can_dlc);
			tmp += 8;
			ret = write(sockfd, &frame, sizeof(frame));
			if(sizeof(frame) != ret) { //如果ret不等于帧长度，就说明发送失败
		        gw_log("can0_write write error");
				perror("write error");
				goto out;
			}
		}
	}

out:
	/* 关闭套接字 */
	close(sockfd);
	exit(EXIT_SUCCESS);
}

static void *can1_write(void *arg)
{
	struct ifreq ifr = {0};
	struct sockaddr_can can_addr = {0};
	struct can_frame frame = {0};
	int sockfd = -1;
	int ret;

	/* 打开套接字 */
	sockfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if(0 > sockfd) {
		gw_log("can1_write socket error");
		perror("socket error");
		exit(EXIT_FAILURE);
	}

	/* 指定can0设备 */
	strcpy(ifr.ifr_name, "can1");
	ioctl(sockfd, SIOCGIFINDEX, &ifr);
	can_addr.can_family = AF_CAN;
	can_addr.can_ifindex = ifr.ifr_ifindex;

	/* 将can0与套接字进行绑定 */
	ret = bind(sockfd, (struct sockaddr *)&can_addr, sizeof(can_addr));
	if (0 > ret) {
		gw_log("can1_write bind error");
		perror("bind error");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	/* 设置过滤规则：不接受任何报文、仅发送数据 */
	setsockopt(sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);

	frame.can_dlc = 8;	//一次发送6个字节数据
	frame.can_id = 0x222;//帧ID为0x123,标准帧

	char *buffer = (char*)calloc(UDP_RECV_BUFFER_SIZE, sizeof(char));
    char *tmp = NULL;
	while(1) {
		memset(buffer, 0, UDP_RECV_BUFFER_SIZE);
		ret = ng_get_udp(buffer);
		if(0 == ret)continue;
		tmp = buffer;

		for(int i=0;i<128;i++){//1024（8*128）个字节的数据）
			/* 发送数据 */
		    memcpy((char*)frame.data, tmp, (size_t)frame.can_dlc);
			ret = write(sockfd, &frame, sizeof(frame));
			if(sizeof(frame) != ret) { //如果ret不等于帧长度，就说明发送失败
				perror("write error");
		        gw_log("can1_write write error");
				goto out;
			}
		}
	}

out:
	/* 关闭套接字 */
	close(sockfd);
	exit(EXIT_SUCCESS);
}

static void *can_state(void *arg)
{
    struct ifreq ifr_can0;
    int s_can0 = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    // 获取can0的接口索引
    memset(&ifr_can0, 0, sizeof(ifr_can0));
    strcpy(ifr_can0.ifr_name, "can0");
    int ret = ioctl(s_can0, SIOCGIFINDEX, &ifr_can0);

    if (ret != 0)
    {
        printf("=> %s\n", strerror(errno));
        goto out;
    }

    struct ifreq ifr_can1;
    int s_can1 = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    // 获取can0的接口索引
    memset(&ifr_can1, 0, sizeof(ifr_can1));
    strcpy(ifr_can1.ifr_name, "can1");
    ret = ioctl(s_can1, SIOCGIFINDEX, &ifr_can1);

    if (ret != 0)
    {
        printf("=> %s\n", strerror(errno));
        goto out;
    }

    int can0_state=0, can1_state=0;
    while(1){
			
		// 获取can0的flags
		ret = ioctl(s_can0, SIOCGIFFLAGS, &ifr_can0);
		if (ret == 0)
		{
			if ((ifr_can0.ifr_flags & (IFF_UP | IFF_RUNNING)) == 0)
			{
				if(1 == can0_state){
					can0_state = 0;
					gw_log("can0_state off");
				}
			}
			else
			{
				if(0 == can0_state){
					can0_state = 1;
					gw_log("can0_state on");
				}
			}
		}
		else
		{
			printf("==> XX %s\n", strerror(errno));
		}

		// 获取can1的flags
		ret = ioctl(s_can1, SIOCGIFFLAGS, &ifr_can1);
		if (ret == 0)
		{
			if ((ifr_can1.ifr_flags & (IFF_UP | IFF_RUNNING)) == 0)
			{
				if(1 == can1_state){
					can1_state = 0;
					gw_log("can1_state off");
				}
			}
			else
			{
				if(0 == can1_state){
					can1_state = 1;
					gw_log("can1_state on");
				}
			}
		}
		else
		{
			printf("==> XX %s\n", strerror(errno));
		}

        sleep(2);
	}
out:
	exit(EXIT_SUCCESS);
}


int main(void){
	/* 初始化互斥锁 */
    pthread_mutex_init(&mutex, NULL);

	pthread_t rCAN0, rCAN1, wCAN0, wCAN1, stCAN, rUDP0, wUDP0;
	int ret;

	ret = pthread_create(&rCAN0, NULL, can0_read, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}
	
	ret = pthread_create(&rCAN1, NULL, can1_read, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}

	ret = pthread_create(&wCAN0, NULL, can0_write, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}
	
	ret = pthread_create(&wCAN1, NULL, can1_write, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}

	ret = pthread_create(&stCAN, NULL, can_state, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}

	ret = pthread_create(&rUDP0, NULL, udp0_read, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}

	ret = pthread_create(&wUDP0, NULL, udp0_write, NULL);
	if (ret) {
		fprintf(stderr, "Error: %s\n", strerror(ret));
		exit(-1);
	}

	pthread_join(rCAN0,NULL);
	pthread_join(rCAN1,NULL);
	pthread_join(wCAN0,NULL);
	pthread_join(wCAN1,NULL);
	pthread_join(stCAN,NULL);
	pthread_join(rUDP0,NULL);
	pthread_join(wUDP0,NULL);
	
	return 0;
}
