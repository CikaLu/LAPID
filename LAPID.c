#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <float.h>
#include <limits.h>
#include <unistd.h>

#include "sal/base/sal_types.h"
#include "sal/base/sal_error.h"
#include "api/port.h"
#include "api/qos.h"

#define package_size 128;
#define bandwidth 1000;
#define CLI_ERROR 1
#define ERROR    1
#define log_level 10
#define QOS_METER_BYTE_MODE 0
#define QOS_METER_PKT_MODE 1
#define PIDWRED 1
#define WRED 0

void debug(uint32_t level, char_t *fmt, ...)
{
    char_t buf[10000];
    va_list ptr;
    if(level <= log_level)
    {

        va_start(ptr, fmt);
        vsprintf(buf, fmt, ptr);
        printf("%s", buf);
        //vprintk(buf, ptr); need to verify.
        va_end(ptr);
    }
}

//默认端口号值
sf_port_t pid = 2;

//报文丢包情况
struct drop_condition
{
    uint64_t sum;
    uint64_t drop_num;
};

//typedef enum cb_table_e{
// CB_PIPE_DROP_CNT0_M,
// CB_IQ_CNT0_M,
// CB_OQ_CNT0_M,
// CB_IDROP_CNT_M,
// CB_ODROP_CNT_M,
// CB_TABLE_MAX
//}cb_table_t;

//此处代表旧状态的一段时间内的丢包情况
struct drop_condition old_old_drop,old_new_drop;
//此处代表新状态的一段时间内的丢包情况

struct drop_condition new_old_drop,new_new_drop;
uint32_t minth = 64;
uint32_t maxth = 200;
uint32_t allth = 264; // minth + maxth
uint32_t refresh_time = 0;
uint32_t weight=3;
uint32_t expth = 132; // 期望的队列长度

float m_top = 1.0;
float m_middle1 = 0.25;
float m_middle2 = 1.0;
float m_middle3 = 1.0;
float m_bottom = 0.0;
float m_alpha = 0.01;
float m_beta;
float beta = 0.97;
//m_part = 0.25 * (m_maxTh - m_minTh);
float m_part = 34.0;
//m_mid = 0.5 * (m_maxTh + m_minTh);
float m_mid = 132.0;
//0.1就是10
float cur_maxp0 = 0.1;
float cur_maxp1 = 0.1;
float m_oldAvg;

typedef struct state_apid_queue
{
    int queue_id;
    float p_max;
    int exp_q_len;
    int q_len;
    double e_t_1;
    double e_t_2;
    float p_t_1;
    double w1;
    double w2;
    double w3;
    float lr;
}st_apid_queue;

typedef struct state_apid_port
{
    st_apid_queue queue_0;
    st_apid_queue queue_1;
    st_apid_queue queue_2;
    st_apid_queue queue_3;
    st_apid_queue queue_4;
    st_apid_queue queue_5;
    st_apid_queue queue_6;
    st_apid_queue queue_7;
}st_apid_port;


// 设置队列WRED的上下门限以及最大丢包率
int32_t cli_qos_egr_wred_control_set(uint32_t queueid, uint32_t drop_rate)
{
	uint32_t pid = 2;
   //printf("enter\n");
    uint32_t    id;
    //sf_port_t   id;
    uint32_t    queue_id;
    // uint8_t     cnt = 0;
    uint32_t    chip_id = 0;
    sf_port_para_t port_status = { 0 };
    sf_status_t ret = SF_E_NONE;
    int sp_or_port = 1;//1代表port  0代表sp 2代表queue
    int q = 2;
    // sf_port_t   chip_port;
   //  printf("chip_port=%d\n", chip_port);

    sf_qos_egrs_ctrl_wred_key_t cfg_key;
    sal_memset(&cfg_key, 0x0, sizeof(sf_qos_egrs_ctrl_wred_key_t));
    cfg_key.cosq_id = SF_QOS_COSQ_INVALID;

    sf_cosq_wred_cfg_t cfg_para;
    sal_memset(&cfg_para, 0x0, sizeof(sf_cosq_wred_cfg_t));

    //printf("enter2\n");

    /* service-pool or port */
    if (sp_or_port == 0) {
        //CLI_GET_INT_PARA(args[cnt + 1], id);
        id = pid;
        if (id > 0) {
            debug(ERROR, "Unsupported service-pool id!(%d)\n", id);
            return SF_E_NOT_SUPPORT;
        }
        // cnt += 2;

        if (sp_or_port == 2) {
            debug(ERROR, "service-pool has no queue !\n");
            return CLI_ERROR;
        }
        cfg_key.wred_type = EGRS_CTRL_BASE_SERVICE_POOL;
        cfg_key.sp_id = id;
    }
    else if (sp_or_port == 1) {
        //CLI_GET_INT_PARA(args[cnt + 1], id);
        id = pid;
       // printf("id=%d\n", id);

      //  printf("cfg_key.port_id=%d\n", cfg_key.port_id);
        //  printf("chip_port=%d\n", chip_port);
        ret = sf_port_panel_to_chip(chip_id, id, &cfg_key.port_id);
        //  printf("chip_port=%d\n", chip_port);
      //  printf("cfg_key.port_id=%d\n", cfg_key.port_id);
        if (ret != SF_E_NONE) {
            debug(ERROR, "Unsupported port ID!(%d)\n", ret);
            return ret;
        }

        /* 不支持cpu和hirar口 */
        SF_E_IF_ERROR_RETURN(sf_panel_port_para_info(chip_id, &port_status));
        if (cfg_key.port_id == port_status.PORT_DEV_CPU_ID || cfg_key.port_id == port_status.PORT_DEV_HIRAR_ID) {
            debug(ERROR, "Unsupported port ID(%d)!\n", id);
            return SF_E_NOT_SUPPORT;
        }

        // cnt += 2;
        cfg_key.wred_type = EGRS_CTRL_BASE_PORT;
    }

   // printf("enter4\n");
    if (q == 2) {
        // CLI_GET_INT_PARA(args[cnt + 1], queue_id);
        queue_id = queueid;
        // cnt += 2;
        cfg_key.wred_type = EGRS_CTRL_BASE_QUEUE;
     //   printf("cfg_key.cosq_id=%d\n", cfg_key.cosq_id);
        ret = sf_qos_internal_queue_id_get(chip_id, cfg_key.port_id, queue_id, &cfg_key.cosq_id);
      //  printf("cfg_key.cosq_id=%d\n", cfg_key.cosq_id);
        if (ret != SF_E_NONE) {
            debug(ERROR, "sf_qos_internal_queue_id_get fail!(%d)\n", ret);
            return ret;
        }
    }


  //  printf("enter5\n");
    ret = sf_cosq_wred_get_ex(chip_id, &cfg_key, &cfg_para);
    if (ret != SF_E_NONE) {
        debug(ERROR, "sf_cosq_wred_get_ex fail!(%d)\n", ret);
        return ret;
    }

    /* refresh time */
  /*if (strcasecmp(args[cnt], "refresh-time") == 0) {
      CLI_GET_INT_PARA(args[cnt+1], cfg_para.refresh_time_sel);
      cnt += 2;
  }*/
    cfg_para.refresh_time_sel = refresh_time;
    //printf("refresh_time=%d\n", cfg_para.refresh_time_sel);
    /* weight val */
    /*if (strcasecmp(args[cnt], "weight") == 0) {
        CLI_GET_INT_PARA(args[cnt+1], cfg_para.weight);
        cnt += 2;
    }*/
    cfg_para.weight = weight;
  //  printf("weight=%d\n", cfg_para.weight);

    uint32_t color = SF_QOS_COLOR_MAX;
    sf_cosq_wred_profile_t entry;

    entry.min_thresh = minth;
   // printf("entry.min_thresh=%d\n", entry.min_thresh);
    entry.max_thresh = maxth;
   // printf("entry.max_thresh=%d\n", entry.max_thresh);
    entry.drop_probability = drop_rate;
   // printf("entry.drop_probability=%d\n", entry.drop_probability);
   // printf("three\n");

    uint32_t _idx;
    for (_idx = SF_QOS_COLOR_GREEN; _idx < SF_QOS_COLOR_MAX; _idx++) {
        if (color != SF_QOS_COLOR_MAX) {
            cfg_para.profile[color].min_thresh = entry.min_thresh;
            cfg_para.profile[color].max_thresh = entry.max_thresh;
            cfg_para.profile[color].drop_probability = entry.drop_probability;
            break;
        }
        cfg_para.profile[_idx].min_thresh = entry.min_thresh;
        cfg_para.profile[_idx].max_thresh = entry.max_thresh;
        cfg_para.profile[_idx].drop_probability = entry.drop_probability;
    }

    return sf_cosq_wred_set_ex(chip_id, &cfg_key, &cfg_para);
}


//  配置队列权重
int32_t queue_wdrr_set(sf_port_t pid, int qid, int weight_value)
{
    sf_port_t   panle_port = pid;//板子编号
    sf_port_t   chip_port;//芯片_端口号
    uint32_t    queue_id = qid;//队列编号
    //之后改为uint32_t unsigned int，因为函数传参要求这个
    uint32_t  weight_val = weight_value;//权重值
    sf_status_t ret = SF_E_NONE;
    uint32_t chip_id = 0;//获取端口号

    //CLI_GET_INT_PARA(pid, panle_port);
    ret = sf_port_panel_to_chip(chip_id, panle_port, &chip_port);
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "Unsupported port ID!(%d)\n",ret);
        return ret;
    }

    //CLI_GET_INT_PARA(qid, queue_id);
    //CLI_GET_INT_PARA(weight_value, weight_val);

    sf_cosq_id_t cosq = SF_QOS_COSQ_INVALID;
    ret = sf_qos_internal_queue_id_get(chip_id, 41, queue_id, &cosq);
    //此方法执行完可以修改cosq值
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "sf_qos_internal_queue_id_get fail!(%d)\n",ret);
        return ret;
    }
    //printf("cosq:%d\n",cosq);

    //weight_val的传值类型是uint32_t unsigned int，可以尝试直接定义成uint32_t
    sf_cosq_schdule_weight_set(chip_id, chip_port, cosq, weight_val);

    return SF_E_NONE;
}

//获取总数和丢包数
sf_status_t cli_counter_drop_rate_get(uint64_t *drop_rate)
{
    uint8_t detail=0,module_name,cnt;
    sf_status_t ret;
    uint32_t chip_id = 0;
    module_name = 2;
    ret = sf_get_drop_rate(chip_id, detail, module_name,2, drop_rate);
    return ret;
}

// 
sf_status_t cli_counter_sum_get(uint64_t *drop_rate)
{
    uint8_t detail=0,module_name,cnt;
    sf_status_t ret;
    uint32_t chip_id = 0;
    module_name = 2;
    ret = sf_get_drop_sum(chip_id, detail, module_name,1, drop_rate);
    return ret;
}

// 入端口队列长度
sf_status_t cli_counter_inner_get(queue_inner *inner)
{
    uint8_t detail=0,module_name,cnt;
    sf_status_t ret;
    uint32_t chip_id = 0;
    module_name = 2;
    ret = sf_get_inner(chip_id, detail, module_name, 1, inner);
    return ret;
}
// 出端口队列长度
sf_status_t cli_counter_egress_queue_get(queue_inner *eq)
{
    uint8_t detail=0,module_name,cnt;
    sf_status_t ret;
    uint32_t chip_id = 0;
    module_name = 2;
    ret = sf_get_egress_queue(chip_id, detail, module_name, 2, eq);
    return ret;
}

// 获取队列权重
sf_status_t get_all_queue_weight(sf_port_t pid)
{

    sf_status_t ret = SF_E_NONE;
    sf_port_t internal_pid;
    ret = sf_port_panel_to_chip(0, pid, &internal_pid);
    if (ret != SF_E_NONE) {
        debug(ERROR, "Unsupported port ID!(%d)\n",ret);
        return ret;
    }

    uint32_t queue_id;
    uint32_t queue_id_max = 8;
    uint32_t weight = 20;

    sf_cosq_id_t cosq = SF_QOS_COSQ_INVALID;
    for (queue_id = 0; queue_id < queue_id_max; queue_id++) {
        (void)sf_qos_internal_queue_id_get(0 ,internal_pid, queue_id, &cosq);
        (void)sf_cosq_schdule_weight_get(0, internal_pid, cosq, &weight);
        printf("queue-%d-weight %d\n",queue_id, weight);
    }
    return ret;
}

sf_status_t get_one_queue_weight(sf_port_t pid,int qid)
{

    sf_status_t ret = SF_E_NONE;
    sf_port_t internal_pid;
    ret = sf_port_panel_to_chip(0, pid, &internal_pid);
    if (ret != SF_E_NONE) {
        debug(ERROR, "Unsupported port ID!(%d)\n",ret);
        return ret;
    }

    uint32_t queue_id = qid;
    uint32_t weight = 20;

    sf_cosq_id_t cosq = SF_QOS_COSQ_INVALID;

    (void)sf_qos_internal_queue_id_get(0 ,internal_pid, queue_id, &cosq);
    (void)sf_cosq_schdule_weight_get(0, internal_pid, cosq, &weight);
    //printf("queue-%d-weight %d\n",queue_id, weight);
    return weight;
}

//设置调度模式
/* 注意和sf_sch_node_id_t对应 */
sf_qos_node_map_t QOS_SCH_NODE_NAME[] = {
    {"s30", SF_QOS_SCH_NODE_S30}, 
    {"s31", SF_QOS_SCH_NODE_S31},
    {"s32", SF_QOS_SCH_NODE_S32},
    {"s33", SF_QOS_SCH_NODE_S33},
    {"s1", SF_QOS_SCH_NODE_S1},
    {"spq", SF_QOS_SCH_NODE_SPQ},
    {"all", SF_QOS_SCH_NODE_MAX},
};

static void _get_sch_node_id_by_name(const char *name, sf_sch_node_id_t *node_id)
{
    *node_id = SF_QOS_SCH_NODE_INVALID;
    uint32_t idx;
    for (idx = 0;idx < sizeof(QOS_SCH_NODE_NAME)/sizeof(QOS_SCH_NODE_NAME[0]);idx++) {
        if (sal_strcmp(name, QOS_SCH_NODE_NAME[idx].node_name) == 0) {
            *node_id = QOS_SCH_NODE_NAME[idx].node_id;
            break;
        }
    }
}


int32_t qos_queue_sch_set(sf_port_t pid, sf_qos_schdule_mode_t shl_mode)
{
    sf_port_t   panle_port;
    sf_status_t ret = SF_E_NONE;
    bool_t      minsp_flag = FALSE;
    uint32_t    chip_id = 0;
    sf_port_para_t port_status = {0};

    sf_qos_sch_cfg_t cfg;
    sal_memset(&cfg, 0x0, sizeof(sf_qos_sch_cfg_t));
        
    
        cfg.port_type = ETH_PORT_TYPE;
        panle_port = pid;
        ret = sf_port_panel_to_chip(chip_id, panle_port, (sf_port_t *)&cfg.port_id);
        if (ret != SF_E_NONE) {
            debug(ERROR, "Unsupported port ID!(%d)\n",panle_port);
            return ret;
        }
        
        SF_E_IF_ERROR_RETURN(sf_panel_port_para_info(chip_id, &port_status));
        if (panle_port < port_status.PORT_DEV_MIN_ID || panle_port > port_status.PORT_DEV_MAX_ID) {
            debug(ERROR, "Unsupported port ID(%d)!\n", panle_port);
            return SF_E_NOT_SUPPORT;
        }

        _get_sch_node_id_by_name("all", &cfg.node_id);
        /* 参数实际合法性判定交给底层判定 */
        if (cfg.node_id == SF_QOS_SCH_NODE_INVALID) {
            debug(ERROR, "eth port error sch node name!\n");
            return 1;
        }

    /* CPU 0-SP调度、1-RR调度 ; ETH 0-SP调度、1-RR调度、2-WDRR调度*/
    sf_qos_schdule_mode_t mode = shl_mode;


    /* minsp-cfg val */
    /*uint32_t minsp_cfg;
    if (cnt < argc) {
        CLI_GET_INT_PARA(args[cnt+1], minsp_cfg);
        minsp_flag = TRUE;
    }*/

    if (cfg.node_id != SF_QOS_SCH_NODE_MAX) {        
        ret = sf_cosq_sch_cfg_get_ex(chip_id, &cfg);
        if (ret != SF_E_NONE) {
            debug(ERROR, "sf_cosq_sch_cfg_get_ex fail!(%d,%d,%d,%d)\n",ret,cfg.port_type,cfg.port_id,cfg.node_id);
            return ret;
        }
        cfg.mode = mode;
        //cfg.minsp_cfg = (minsp_flag) ? (minsp_cfg) : (cfg.minsp_cfg);
        ret = sf_cosq_sch_cfg_set_ex(chip_id, &cfg);
        return ret;
    }

    sf_sch_node_id_t idx;
    for (idx = SF_QOS_SCH_NODE_S30; idx < SF_QOS_SCH_NODE_SPQ; idx++) {

        if (cfg.port_type == ETH_PORT_TYPE && idx != SF_QOS_SCH_NODE_S30 && idx != SF_QOS_SCH_NODE_S31 &&
            idx != SF_QOS_SCH_NODE_S1) {
            continue;
        }

        /* 基于端口针对所有调度节点的配置 */
        cfg.node_id = idx;
        ret = sf_cosq_sch_cfg_get_ex(chip_id, &cfg);
        if (ret != SF_E_NONE) {
            debug(ERROR, "sf_cosq_sch_cfg_get_ex fail!(%d,%d,%d,%d)\n",ret,cfg.port_type,cfg.port_id,cfg.node_id);
            return ret;
        }
        
        //cfg.minsp_cfg = (minsp_flag) ? (minsp_cfg) : (cfg.minsp_cfg);
        cfg.mode = mode;
        ret = sf_cosq_sch_cfg_set_ex(chip_id, &cfg);
        if (ret != SF_E_NONE) {
            debug(ERROR, "sf_cosq_sch_cfg_set_ex fail!(%d,%d,%d,%d)\n",ret,cfg.port_type,cfg.port_id,cfg.node_id);
            return ret;
        }
    }    
    return ret;
}

static int32_t _qos_queue_sch_enable_disable(sf_port_t pid, int qid, bool_t status)
{
    sf_port_t   panle_port;
    sf_port_t   chip_port;
    uint32_t    port_type;
    uint32_t    queue_id;
    sf_status_t ret = SF_E_NONE;
    uint32_t    chip_id = 0;
    sf_port_para_t port_status = {0};

    /* cpu-port or eth-port 
        0:以太端口
    */
    port_type = 0;

    panle_port = pid;
    ret = sf_port_panel_to_chip(chip_id, panle_port, &chip_port);
    if (ret != SF_E_NONE) {
        debug(ERROR, "Unsupported port ID!(%d)\n",ret);
        return ret;
    }

    /* 不支持非以太口 */
    SF_E_IF_ERROR_RETURN(sf_panel_port_para_info(chip_id, &port_status));
    if (chip_port == port_status.PORT_DEV_HIRAR_ID || chip_port == port_status.PORT_DEV_CPU_ID) {
        debug(ERROR, "Unsupported port ID(%d)!\n", panle_port);
        return SF_E_NOT_SUPPORT;
    }
    
    queue_id = qid;

    bool_t re_status;
    ret = sf_cosq_sch_en_get(chip_id, port_type, chip_port, queue_id, &re_status);
    if (ret != SF_E_NONE) {
        debug(ERROR, "sf_cosq_sch_en_get fail!(%d)\n",ret);
        return ret;
    }

    if (re_status == status) {
        return SF_E_NONE;
    }
    
    return sf_cosq_sch_en_set(chip_id, port_type, chip_port, queue_id, status);
}


/* qos queue-sch-en (cpu-port | eth-port) &lt;1-56&gt; queue &lt;0-31&gt; enable */
int32_t qos_queue_sch_enable(sf_port_t pid, int qid)
{
    return _qos_queue_sch_enable_disable(pid,qid,1);
}

// 获取当前队列长度
// int getcosq(uint32_t queue_id)
// {
// 	sf_qos_egrs_ctrl_stat_t stat;
// 	memset(&stat, 0x0, sizeof(sf_qos_egrs_ctrl_stat_t));
// 	stat.cosq_id = SF_QOS_COSQ_INVALID;
// 	stat.port_id = 41;
// 	stat.stat_type = EGRS_CTRL_BASE_QUEUE;
// 	sf_qos_internal_queue_id_get(0, stat.port_id, queue_id, &stat.cosq_id);
// 	sf_qos_egrs_ctrl_stat_get(0, &stat);
// 	/*printf("port_id: %d\n", stat.port_id);
// 	printf("queue_id: %d\n", queue_id);
// 	printf("cosq_id: %d\n", stat.cosq_id);
// 	printf("cosq_all_cnt: %d\n", stat.cosq_all_cnt);*/
// 	return stat.cosq_all_cnt;
// }


int getcosq(sf_port_t pid, int qid)
{
    sf_port_t   chip_port;
    uint32_t    id, queue_id;
    uint32_t    chip_id = 0;
    sf_status_t ret = SF_E_NONE;

    sf_qos_egrs_ctrl_cfg_t cfg;
    sal_memset(&cfg, 0x0, sizeof(sf_qos_egrs_ctrl_cfg_t));
    cfg.cosq_id = SF_QOS_COSQ_INVALID;

    sf_qos_egrs_ctrl_stat_t stat;
    sal_memset(&stat, 0x0, sizeof(sf_qos_egrs_ctrl_stat_t));
    stat.cosq_id = SF_QOS_COSQ_INVALID;

    id = pid;
    ret = sf_port_panel_to_chip(chip_id, id, &chip_port);
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "Unsupported port ID!(%d)\n",ret);
        return ret;
    }

    cfg.port_id = chip_port;
    cfg.sp_id = 0;
    cfg.cfg_type = EGRS_CTRL_BASE_PORT;
    stat.port_id = chip_port;
    stat.sp_id = 0;
    stat.stat_type = EGRS_CTRL_BASE_PORT;

    queue_id = qid;
    cfg.cfg_type = EGRS_CTRL_BASE_QUEUE;
    stat.stat_type = EGRS_CTRL_BASE_QUEUE;
    sf_cosq_id_t cosq_id = 0;
    ret = sf_qos_internal_queue_id_get(chip_id, chip_port, queue_id, &cosq_id);
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "sf_qos_internal_queue_id_get fail!(%d)\n",ret);
        return ret;
    }
    cfg.cosq_id = cosq_id;
    stat.cosq_id = cosq_id;

    ret = sf_qos_egrs_ctrl_cfg_get(chip_id, &cfg);
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "sf_qos_egrs_ctrl_cfg_get fail!(%d)\n",ret);
        return ret;
    }

    uint16_t cosq_min = cfg.cosq_min;
    uint16_t share = cfg.threshold[SF_QOS_COLOR_MAX].lim_thd;

    ret = sf_qos_egrs_ctrl_stat_get(chip_id, &stat);
    if (ret != SF_E_NONE)
    {
        debug(ERROR, "sf_qos_egrs_ctrl_stat_get fail!(%d)\n",ret);
        return ret;
    }

    uint32_t cosq_all_cnt  = stat.cosq_all_cnt;
    return cosq_all_cnt;
}


// 计算队列长度的移动加权平均长度
// 获取移动加权平均队列长度
// 获取RED算法的当前丢包率
float get_cur_p(int cur_q, float cur_maxp)
{
    if (cur_q < minth){
        return 0;
    }
    else if (cur_q > maxth){
        return 1.0;
    }
    else{
        return ((cur_q - minth) * 1.0 / (maxth - minth) * 1.0 * cur_maxp);
    }
}

// sign函数
int sign(float fz, float fm)
{
    if ((fz > 0 && fm >= 0) || (fz < 0 && fm <= 0)){
        return 1;
    }
    else if ((fz < 0 && fm >=0) || (fz > 0 && fm <= 0)){
        return -1;
    }
    else if (fz == 0){
        return 0;
    }
}

int regular_prob(float max_prob){
    int p = (int)(max_prob * 100);
    if (p <= 0){
        return 0;
    }
    else if (p <= 10) {
        return p;
    }
    else if (p <= 25) {
        return 25;
    }
    else if (p <= 50) {
        return 50;
    }
    else if (p <= 75) {
        return 75;
    }
    else{
        return 100;
    }
}


// 获取新阶段的最大丢包率
float updateMaxp(st_apid_queue *last_st_q, int cur_q, int learning_enable)
{
	float cur_e, cur_maxp, cur_p, z, delta_p;
    float x1, x2, x3;
    cur_e = (cur_q - last_st_q->exp_q_len) * 1.0 / allth;
    printf("Cur_e : %.8f \t\n", cur_e);
    /*
        当 cur_e < 0, 表示 当前的队列长度 < 期望长度； 丢包率没有必要增加； 所以 delta_p <= 0;
        当 cur_e = 0, 表示 当前的队列长度 = 期望长度； 丢包率没有必要增加； 所以 delta_p ~= 0;
        当 cur_e > 0, 表示 当前的队列长度 > 期望长度； 丢包率必须要增加；   所以 delta_p >= 0;
        PID增量计算公式：
        w1 = kp + ki + kd;  w2 = - (2kd + kp);  w3 = kd;
        所以权重的约束为 w1 >= 0, w2 <= 0, w3 >= 0；
        设 y1 = w1 * cur_e;  y2 = w2 * e_t_1;  y3 = w3 * e_t_2;
        假设队列处于上升趋势时: 
        当 cur_e < 0 时, 需要 delta_p <= 0，此时 y1 <= 0; y2 >= 0; y3 <= 0; 所以，-(y1 + y3) >= y3; 
        当 cur_e = 0 时, 需要 delta_p ~= 0，此时 y1 = 0; y2 >= 0; y3 <= 0; 所以， y2 ~= -y3;
        当 cur_e > 0 时, 需要 delta_p >= 0，此时 y1 >= 0; y2 <= 0; y3 >= 0; 所以，y1 + y3 >= -y3; 
        由于 cur_e 是当前队列变化最直接的表示，所以 w1 需要足够大才能对算法整体起作用。
    */  
    // 计算增量
    delta_p = (last_st_q->w1 * cur_e) + (last_st_q->w2 * last_st_q->e_t_1) + (last_st_q->w3 * last_st_q->e_t_2);
    printf("Addition : %.8f \t\n", delta_p);
    // 获取新的最大丢包率
    cur_maxp = last_st_q->p_max + delta_p;
    // 限制cur_maxp在0~1之间, 使用0.01和0.99进行平滑
    if (cur_maxp < 0.01) {
        cur_maxp = 0.00;
    }
    if (cur_maxp > 0.99) {
        cur_maxp = 1.0;
    }
    // printf("Max_p : %.8f \t\n", cur_maxp);
    // 获取当前的丢包率
    cur_p = get_cur_p(cur_q, (float)regular_prob(cur_maxp) / 100.0);
    printf("Cur_p : %.8f \t\n\n", cur_p);
    // 更新权重
    float fz = (cur_q - last_st_q->q_len);
    float fm = (cur_p - last_st_q->p_t_1);
    z = (float)sign(fz ,fm);
    if (learning_enable == 1){
        last_st_q->w1 = last_st_q->w1 - last_st_q->lr * cur_e * z * cur_e;
        last_st_q->w2 = last_st_q->w2 - last_st_q->lr * cur_e * z * last_st_q->e_t_1;
        last_st_q->w3 = last_st_q->w3 - last_st_q->lr * cur_e * z * last_st_q->e_t_2;
        // last_st_q->lr = last_st_q->lr * 0.998;
        if (last_st_q->w1 <= 0){
            last_st_q->w1 = 0.0;
        }
        if (last_st_q->w2 >= 0){
            last_st_q->w2 = 0.0;
        }
        if (last_st_q->w3 <= 0){
            last_st_q->w3 = 0.0;
        }
    }
    // 更新暂存状态
    last_st_q->p_max = cur_maxp;
    last_st_q->e_t_2 = last_st_q->e_t_1;
    last_st_q->e_t_1 = cur_e;
    last_st_q->p_t_1 = cur_p;
    last_st_q->q_len = cur_q;
    // printf("Max_p in Queue : %.8f \t\n", last_st_q->p_max);
    return cur_maxp;
}

static void print_queue_pid_parameter(st_apid_queue *last_st_q)
{
    printf("============================\n");
    printf("Queue ID:          %d\t\n", last_st_q->queue_id);
    printf("Last Max P:        %.4f\t\n", last_st_q->p_max);
    printf("Last P:            %.4f\t\n", last_st_q->p_t_1);
    printf("Exp Len of Queue:  %d\t\n", last_st_q->exp_q_len);
    printf("Lst Len of Queue:  %d\t\n", last_st_q->q_len);
    printf("Error(t-1):        %.8f\t\n", last_st_q->e_t_1);
    printf("Error(t-2):        %.8f\t\n", last_st_q->e_t_2);
    printf("Weight 1:          %.8f\t\n", last_st_q->w1);
    printf("Weight 2:          %.8f\t\n", last_st_q->w2);
    printf("Weight 3:          %.8f\t\n", last_st_q->w3);
    printf("Learning Rate:     %.8f\t\n", last_st_q->lr);
    printf("============================\n");
}

void init_pid_port(st_apid_port *port, int a, int b, int c)
{
    /* data */
    st_apid_queue *last_st_q = port;
    int i;
    for(i = 0; i < 8; i++){
        (last_st_q+i)->queue_id = i;
        (last_st_q+i)->p_max = 0.01;
        (last_st_q+i)->p_t_1 = 0.0;
        (last_st_q+i)->exp_q_len = expth;
        (last_st_q+i)->q_len = expth;
        (last_st_q+i)->e_t_1 = 0.0;
        (last_st_q+i)->e_t_2 = 0.0;
        (last_st_q+i)->w1 = a * 0.001;
        (last_st_q+i)->w2 = b * -0.0001;
        (last_st_q+i)->w3 = c * 0.00001;
        (last_st_q+i)->lr = 0.002;
    }
}

void usage() {
    printf("Usage:\n");
    printf("\tApid-red [-r] [-q number] [-d mode] [-w weight] [-l learning] [-a w1] [-b w2] [-c w3] [-e len]\n");
    printf("\t\t[-r] use wred only; defalut is False;\n");
    printf("\t\t[-q] the number of using queues; defalut is 1 and queue 0;\n");
    printf("\t\t[-d] scheduling mode: 0 SP, 1 RR, 2 WDRR; defalut is 0 SP;\n");
    printf("\t\t[-w] weight of avg; defalut is 9;\n");
    printf("\t\t[-l] enable leanring ; defalut is enable;\n");
    printf("\t\t[-a] a * 0.001 = w1; defalut is 1;\n");
    printf("\t\t[-b] b * -0.0001 = w2; defalut is 1;\n");
    printf("\t\t[-c] c * 0.00001 = w3; defalut is 1;\n");
    printf("\t\t[-e] expect the len of queue; defalut is 132;\n");
}

float cal_avg_queue(st_apid_queue *queue, int cosq)
{
    float w = pow(0.5, weight);
    return queue->q_len * (1.0 - w) + cosq * 1.0 * w;
    // return (queue->q_len * 0.875 + cosq * 0.125);
}

int main(int argc, char* argv[])
{   
    int wred_only = 0;
    int learning_enable = 1;
    int num_of_queues = 1;
    int scheduling_mode = 0;
    int o;
    int a = 1, b = 1, c = 1;
    int print_flag = 0;
    const char *optstring = "rlq:d:w:a:b:c:e:";
    while ((o = getopt(argc, argv, optstring)) != -1) {
        switch (o) {
            case 'r':
                wred_only = 1;
                print_flag = PIDWRED;
                break;
            case 'l':
                learning_enable = 0;
                break;
            case 'q':
                if (atoi(optarg) < 8 && atoi(optarg) > 0){
                    num_of_queues = atoi(optarg);
                    printf("num_of_queues is %d\n", num_of_queues);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'd':
                if (atoi(optarg) < 3 && atoi(optarg) >= 0){
                    scheduling_mode = atoi(optarg);
                    printf("scheduling_mode is %d\n", scheduling_mode);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'w':
                if (atoi(optarg) >= 0){
                    weight = atoi(optarg);
                    printf("weight of avg is %d\n", weight);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'a':
                if (atoi(optarg) >= 0){
                    a = atoi(optarg);
                    printf("w1 is %.4f\n", a * 0.001);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'b':
                if (atoi(optarg) >= 0){
                    b = atoi(optarg);
                    printf("w2 is %.5f\n", b * 0.0001);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'c':
                if (atoi(optarg) >= 0){
                    c = atoi(optarg);
                    printf("w3 is %.6f\n", c * 0.00001);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'e':
                if (atoi(optarg) >= 0){
                    expth = atoi(optarg);
                    printf("expect len of queue %d\n", expth);
                }
                else{
                    usage();
                    return -1;
                }
                break;
            case 'h':
                usage();
                return 0;
            case '?':
                printf("error optopt: %c\n", optopt);
                usage();
                return -1;
        }
    }

    printf("-------Step 1  Init product-------\n");
    sfui_comm_init();  // init sdk
	uint32_t bandwidth_mode = QOS_METER_BYTE_MODE;
	uint32_t chip_id = 0;
    // 0 SP 1 RR 2 WDRR 设置队列调度算法
    qos_queue_sch_set(pid, scheduling_mode);
    // diag_shell 设置出口带宽限制
    //31%
    // uint64_t c_q0 = 284208000;
    // uint64_t c_q1 = 15784000;
    //48%
    //uint64_t c_q0 = 435784000;
    //uint64_t c_q1 = 24208000;
    //79%
    //uint64_t c_q0 = 710520000;
    //uint64_t c_q1 = 39472000;


    printf("-------Step 2  Init parameter-------\n");
    // 判断argc是不是数字

    int round_sum = 1000;
    // 从端口1进，端口2出
    //  init the pid parameter for port 2 
    st_apid_port apid_port_2;
    st_apid_queue *_apid_queue = &apid_port_2;
    init_pid_port(&apid_port_2, a, b, c);
    
    int cosq = 0;

    printf("-------Step 3  Start APID-RED For %d-------\n", round_sum);
    int i, j;
    for (i = 0; i <= round_sum; i++)
    {
        sleep(1);
        printf("********  [Round %d / %d]  **********\n", i, round_sum);
        int drop_sum, pkg_sum;
        cli_counter_drop_rate_get(&drop_sum);
        cli_counter_sum_get(&pkg_sum);

        float cur_maxp = 0.0;
        int drop = 0;
        float avg = 64.0;     // 平均加权队列长度
        
        // 获取当前的队列平均长度
        for (j = 0; j < num_of_queues; j++){

            cosq = getcosq(pid, j); 
            if(print_flag == 1) {
                FILE* const file1 = fopen("pid_wred.txt", "a+");//w  写入
                fprintf(file1, "%f\n", cosq);
                fclose(file1);
            } else {
                FILE* const file2 = fopen("wred_only.txt", "a+");//w  写入
                fprintf(file2, "%f\n", cosq);
                fclose(file2);
            }
            avg = cal_avg_queue(_apid_queue+j, cosq);
            
            if (wred_only == 1){
                (_apid_queue+j)->q_len = avg;
                cur_maxp = 100.0;
            }else{
                print_queue_pid_parameter(_apid_queue+j);
                cur_maxp = updateMaxp(_apid_queue+j, avg, learning_enable);
            }
            drop = regular_prob(cur_maxp);
            cli_qos_egr_wred_control_set(j, drop);
            
            printf("Cosq: %d \t\nAvgq: %.4f \t\n\n", cosq, avg);
        }
        printf("****************************\n");
    }

    return 0;
}
