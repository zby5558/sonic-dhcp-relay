# Sonic DHCP Relay - 压力测试和DHCPv6 SOLICIT 分析

## 总体概述

在 sonic-dhcp-relay repo 中发现了完整的压力测试(Stress Test)实现和DHCPv6 SOLICIT消息处理。主要分布在以下文件中：

- **src/relay.cpp**: 包含压力测试核心实现
- **src/relay.h**: 数据结构定义和接口声明
- **test/mock_relay.cpp**: SOLICIT消息的mock测试数据

---

## 一、压力测试(Stress Test)实现

### 1. 压力测试数据结构

```cpp
struct stress_test_args {
    std::unordered_map<std::string, relay_config> *vlans;
    int rate;              // 压力测试包速率(pps - packets per second)
    int count;             // 压力测试包总数(0表示无限)
    int sent;              // 已发送包数
    struct event *timer_ev; // libevent timer
};
```

### 2. DHCPv6 SOLICIT消息构造函数

**函数签名：**
```cpp
void generate_fake_solicit(uint8_t *buf, uint16_t &len, const uint8_t *mac)
```

**实现细节：**
- 构造虚假的DHCPv6 SOLICIT消息
- msg_type = 1 (SOLICIT)
- 生成随机的transaction ID (3字节)
- 包含以下DHCPv6选项：

#### 选项1：Client ID (Option 1)
```
- Type: 1 (Client Identifier)
- Length: 8 bytes (DUID-LL)
- DUID Type: 3 (DUID-LL)
- Hardware Type: 1 (Ethernet)
- MAC Address: 6 bytes (由参数mac提供)
```

#### 选项3：IA_NA (Option 3)
```
- Type: 3 (Identity Association for Non-temporary Address)
- Length: 12 bytes
- IAID: 4 bytes (随机)
- T1 (renewal time): 4 bytes (0)
- T2 (rebind time): 4 bytes (0)
```

**完整代码：**
```cpp
void generate_fake_solicit(uint8_t *buf, uint16_t &len, const uint8_t *mac) {
    buf[0] = 1; // msg_type: Solicit
    buf[1] = rand() % 256;
    buf[2] = rand() % 256;
    buf[3] = rand() % 256;
    
    uint16_t offset = 4;
    
    // Client ID Option (Option 1)
    uint16_t opt_clientid_type = htons(1);
    uint16_t opt_clientid_len = htons(8); // DUID-LL len
    std::memcpy(buf + offset, &opt_clientid_type, 2);
    std::memcpy(buf + offset + 2, &opt_clientid_len, 2);
    
    uint16_t duid_type = htons(3); // DUID-LL
    uint16_t hw_type = htons(1); // Ethernet
    std::memcpy(buf + offset + 4, &duid_type, 2);
    std::memcpy(buf + offset + 6, &hw_type, 2);
    std::memcpy(buf + offset + 8, mac, 6);
    
    offset += 14;
    
    // IA_NA Option (Option 3)
    uint16_t opt_iana_type = htons(3);
    uint16_t opt_iana_len = htons(12);
    std::memcpy(buf + offset, &opt_iana_type, 2);
    std::memcpy(buf + offset + 2, &opt_iana_len, 2);
    
    uint32_t iaid = rand();
    uint32_t t1 = 0;
    uint32_t t2 = 0;
    std::memcpy(buf + offset + 4, &iaid, 4);
    std::memcpy(buf + offset + 8, &t1, 4);
    std::memcpy(buf + offset + 12, &t2, 4);
    
    offset += 16;
    
    len = offset;
}
```

### 3. 压力测试回调函数

**函数签名：**
```cpp
void stress_test_callback(evutil_socket_t fd, short event, void *arg)
```

**工作原理：**
- 使用累加器控制包速率(rate pps with 10ms timer)
- 每隔10ms触发一次回调
- 支持两种运行模式：
  - 无计数限制：stress_rate > 0, stress_count = 0 (持续发送)
  - 有计数限制：stress_rate > 0, stress_count > 0 (发送固定数量)

**完整代码：**
```cpp
void stress_test_callback(evutil_socket_t fd, short event, void *arg) {
    auto args = reinterpret_cast<stress_test_args *>(arg);
    
    static double accumulator = 0;
    double interval_sec = 0.010; // 10ms
    accumulator += args->rate * interval_sec;
    int pkts_to_send = (int)accumulator;
    accumulator -= pkts_to_send;
    
    if (args->count > 0 && args->sent >= args->count) {
        syslog(LOG_INFO, "Stress test finished. Sent %d packets.", args->sent);
        event_del(args->timer_ev);
        return;
    }
    
    for (int i = 0; i < pkts_to_send; i++) {
        if (args->count > 0 && args->sent >= args->count) {
            break;
        }
        
        // Find a ready VLAN
        struct relay_config *chosen_vlan = nullptr;
        for (auto &kv : *(args->vlans)) {
            if (kv.second.is_lla_ready && !kv.second.servers_sock.empty() && kv.second.gua_sock > 0) {
                chosen_vlan = &kv.second;
                break;
            }
        }
        
        if (!chosen_vlan) {
            return;
        }
        
        // Generate fake MAC
        uint8_t mac[6];
        for (int j = 0; j < 6; j++) mac[j] = rand() % 256;
        mac[0] &= 0xfe; // Unicast
        mac[0] |= 0x02; // Locally administered
        
        // Generate fake Solicit
        uint8_t solicit_buf[256];
        uint16_t solicit_len = 0;
        generate_fake_solicit(solicit_buf, solicit_len, mac);
        
        // Generate fake IPv6 Src
        struct in6_addr peer_addr;
        std::memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.s6_addr[0] = 0xfe;
        peer_addr.s6_addr[1] = 0x80;
        for (int j = 8; j < 16; j++) peer_addr.s6_addr[j] = rand() % 256;
        
        // Construct RelayMsg
        RelayMsg relay;
        relay.m_msg_hdr.msg_type = DHCPv6_MESSAGE_TYPE_RELAY_FORW;
        relay.m_msg_hdr.hop_count = 0;
        std::memcpy(&relay.m_msg_hdr.peer_address, &peer_addr, sizeof(in6_addr));
        std::memcpy(&relay.m_msg_hdr.link_address, &chosen_vlan->link_address.sin6_addr, sizeof(in6_addr));
        
        if (chosen_vlan->is_option_79) {
            option_linklayer_addr option79;
            option79.link_layer_type = htons(1);
            std::memcpy(option79.link_layer_addr, mac, 6);
            relay.m_option_list.Add(OPTION_CLIENT_LINKLAYER_ADDR, (const uint8_t *)&option79, sizeof(option_linklayer_addr));
        }
        
        if (chosen_vlan->is_interface_id) {
            option_interface_id intf_id;
            intf_id.interface_id = chosen_vlan->link_address.sin6_addr;
            relay.m_option_list.Add(OPTION_INTERFACE_ID, (const uint8_t *)&intf_id, sizeof(option_interface_id));
        }
        
        relay.m_option_list.Add(OPTION_RELAY_MSG, solicit_buf, solicit_len);
        
        uint16_t relay_pkt_len = 0;
        auto relay_pkt = relay.MarshalBinary(relay_pkt_len);
        if (relay_pkt && relay_pkt_len > 0) {
            int sock = chosen_vlan->gua_sock;
            if (dual_tor_sock) {
                sock = chosen_vlan->lo_sock;
            }
            for (auto server : chosen_vlan->servers_sock) {
                send_udp(sock, relay_pkt, server, relay_pkt_len);
            }
            args->sent++;
        }
    }
}
```

### 4. 压力测试启动和配置

**函数签名：**
```cpp
void loop_relay(std::unordered_map<std::string, relay_config> &vlans, int stress_rate = 0, int stress_count = 0)
```

**启动压力测试的代码片段：**
```cpp
// If stress rate is enabled, set up stress test timer
if (stress_rate > 0) {
    struct event *stress_timer;
    struct timeval stress_tv;
    auto stress_args = new stress_test_args();
    stress_args->vlans = &vlans;
    stress_args->rate = stress_rate;
    stress_args->count = stress_count;
    stress_args->sent = 0;
    stress_args->timer_ev = nullptr;

    stress_timer = event_new(base, -1, EV_PERSIST, stress_test_callback, stress_args);
    stress_args->timer_ev = stress_timer;

    stress_tv.tv_sec = 0;
    stress_tv.tv_usec = 10000; // 10ms
    event_add(stress_timer, &stress_tv);
    syslog(LOG_INFO, "Stress test timer added: rate %d pps, count %d", stress_rate, stress_count);
}
```

---

## 二、DHCPv6 消息类型定义

```cpp
typedef enum {
    DHCPv6_MESSAGE_TYPE_UNKNOWN = 0,
    DHCPv6_MESSAGE_TYPE_SOLICIT = 1,              // ← 关键
    DHCPv6_MESSAGE_TYPE_ADVERTISE = 2,
    DHCPv6_MESSAGE_TYPE_REQUEST = 3,
    DHCPv6_MESSAGE_TYPE_CONFIRM = 4,
    DHCPv6_MESSAGE_TYPE_RENEW = 5,
    DHCPv6_MESSAGE_TYPE_REBIND = 6,
    DHCPv6_MESSAGE_TYPE_REPLY = 7,
    DHCPv6_MESSAGE_TYPE_RELEASE = 8,
    DHCPv6_MESSAGE_TYPE_DECLINE = 9,
    DHCPv6_MESSAGE_TYPE_RECONFIGURE = 10,
    DHCPv6_MESSAGE_TYPE_INFORMATION_REQUEST = 11,
    DHCPv6_MESSAGE_TYPE_RELAY_FORW = 12,         // Relay Forward
    DHCPv6_MESSAGE_TYPE_RELAY_REPL = 13,         // Relay Reply
    DHCPv6_MESSAGE_TYPE_MALFORMED = 14,
    DHCPv6_MESSAGE_TYPE_COUNT
} dhcp_message_type_t;
```

---

## 三、DHCPv6 选项常量

```cpp
#define OPTION_RELAY_MSG 9                // Option code for Relay Message
#define OPTION_INTERFACE_ID 18            // Option code for Interface ID
#define OPTION_CLIENT_LINKLAYER_ADDR 79   // Option code for Client Link-Layer Address
```

---

## 四、关键数据结构

### 4.1 relay_config 结构

```cpp
struct relay_config {
    int gua_sock;                                    // Global Unicast Address socket
    int lla_sock;                                    // Link-Local Address socket
    int lo_sock;                                     // Loopback socket (for dual-tor)
    int filter;                                      // Berkeley Packet Filter
    sockaddr_in6 link_address;                       // Relay's link address
    std::shared_ptr<swss::DBConnector> state_db;    // State database connector
    std::string interface;                           // Interface name
    std::string mux_key;                             // Mux cable key
    std::vector<std::string> servers;                // DHCP server addresses
    std::vector<sockaddr_in6> servers_sock;          // DHCP server sockets
    bool is_option_79;                               // Support Option 79 (Link-Layer Address)
    bool is_interface_id;                            // Support Option 18 (Interface ID)
    std::shared_ptr<swss::Table> mux_table;          // Mux cable table
    std::shared_ptr<swss::DBConnector> config_db;    // Config database connector
    bool is_lla_ready;                               // LLA ready flag
};
```

### 4.2 DHCPv6 消息结构

```cpp
struct PACKED dhcpv6_msg {
    uint8_t msg_type;
    uint8_t xid[3];              // Transaction ID (3 bytes)
};

struct PACKED dhcpv6_relay_msg {
    uint8_t msg_type;
    uint8_t hop_count;
    struct in6_addr link_address;
    struct in6_addr peer_address;
};

struct PACKED dhcpv6_option {
    uint16_t option_code;
    uint16_t option_length;
};

struct PACKED option_linklayer_addr {
    uint16_t link_layer_type;
    uint8_t link_layer_addr[6];    // MAC address
};

struct PACKED option_interface_id {
    in6_addr interface_id;         // Interface's global IPv6 address
};
```

---

## 五、测试中的 Mock SOLICIT 消息

### 5.1 原始SOLICIT包(hex dump)

```cpp
uint8_t client_raw_solicit[] = {
    0x33, 0x33, 0x00, 0x01, 0x00, 0x02, 0x08, 0x00, 0x27, 0xfe, 0x8f, 0x95, 0x86, 0xdd, 0x60, 0x00,
    0x00, 0x00, 0x00, 0x3c, 0x11, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x27, 0xff, 0xfe, 0xfe, 0x8f, 0x95, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x02, 0x22, 0x02, 0x23, 0x00, 0x3c, 0xad, 0x08, 0x01, 0x10,
    0x08, 0x74, 0x00, 0x01, 0x00, 0x0e, 0x00, 0x01, 0x00, 0x01, 0x1c, 0x39, 0xcf, 0x88, 0x08, 0x00,
    0x27, 0xfe, 0x8f, 0x95, 0x00, 0x06, 0x00, 0x04, 0x00, 0x17, 0x00, 0x18, 0x00, 0x08, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x19, 0x00, 0x0c, 0x27, 0xfe, 0x8f, 0x95, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x00,
    0x15, 0x18
};
```

**包层级分析：**
- 以太网帧 (14字节)
- IPv6头 (40字节)
- UDP头 (8字节)
- DHCPv6 SOLICIT消息

### 5.2 带扩展头的SOLICIT包

```cpp
uint8_t client_raw_solicit_with_externsion[] = {
    0x33, 0x33, 0x00, 0x01, 0x00, 0x02, 0x08, 0x00, 0x27, 0xfe, 0x8f, 0x95, 0x86, 0xdd, 0x60, 0x00,
    0x00, 0x00, 0x00, 0x44, 0x2c, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x27, 0xff, 0xfe, 0xfe, 0x8f, 0x95, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x02, 0x22, 0x02, 0x23, 0x00, 0x3c, 0xad, 0x08, 0x01, 0x10,
    0x08, 0x74, 0x00, 0x01, 0x00, 0x0e, 0x00, 0x01, 0x00, 0x01, 0x1c, 0x39, 0xcf, 0x88, 0x08, 0x00,
    0x27, 0xfe, 0x8f, 0x95, 0x00, 0x06, 0x00, 0x04, 0x00, 0x17, 0x00, 0x18, 0x00, 0x08, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x19, 0x00, 0x0c, 0x27, 0xfe, 0x8f, 0x95, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x00,
    0x15, 0x18
};
```

### 5.3 DHCPv6 SOLICIT消息序列化测试

```cpp
uint8_t solicit[] = {
    0x01, 0x00, 0x30, 0x39, 0x00, 0x01, 0x00, 0x0e, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x98, 0x03, 0x9b, 0x03, 0x22, 0x01, 0x00, 0x06, 0x00, 0x06, 0x00, 0x17, 0x00, 0x18, 0x00, 0x1d,
    0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
```

---

## 六、SOLICIT 计数器集成

### 6.1 计数器初始化

```cpp
initialize_counter(state_db, ifname);
// 初始化所有消息类型的计数器：
// - Solicit (msg_type=1)
// - Advertise (msg_type=2)
// - Request, Confirm, Renew, Rebind, Reply, Release, Decline
// - Relay-Forward, Relay-Reply
// - Unknown
```

### 6.2 计数器增加

```cpp
state_db->hset("DHCPv6_COUNTER_TABLE|Vlan1000", "Solicit", "0");
increase_counter(state_db, ifname, 1);  // Increment Solicit counter
std::shared_ptr<std::string> output = state_db->hget("DHCPv6_COUNTER_TABLE|Vlan1000", "Solicit");
```

**数据库位置：**
```
Key: DHCPv6_COUNTER_TABLE|{VLAN_NAME}
Field: Solicit
Value: <counter_value>
```

---

## 七、使用示例

### 启用压力测试的命令行参数

根据代码逻辑推断，主程序 `main()` 应该接受：

```
stress_rate: 指定包速率(pps)，例如 1000 = 1000 packets/second
stress_count: 指定总包数，0 = 无限发送

// 假设用法（伪代码）
loop_relay(vlans, stress_rate=1000, stress_count=10000);  // 发送10000个1000pps的包
loop_relay(vlans, stress_rate=5000, stress_count=0);      // 连续以5000pps发送
```

---

## 八、关键流程总结

```
压力测试流程图：
                    ┌─────────────────────┐
                    │ loop_relay()启动   │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │ 检查stress_rate    │
                    │ (> 0?)            │
                    └──────────┬──────────┘
                               │ Yes
                    ┌──────────▼──────────────────┐
                    │ 创建stress_timer事件       │
                    │ 周期: 10ms (EV_PERSIST)    │
                    └──────────┬──────────────────┘
                               │
                    ┌──────────▼──────────────────┐
              ┌─────│ 每10ms: stress_test_callback│
              │     └──────────┬──────────────────┘
              │                │
              │     ┌──────────▼──────────────────┐
              │     │ 计算要发送的包数           │
              │     │ (基于rate和accumulator)  │
              │     └──────────┬──────────────────┘
              │                │
              │     ┌──────────▼──────────────────┐
              │     │ For each packet:           │
              │     │ 1. 选择就绪的VLAN         │
              │     │ 2. 生成随机MAC           │
              │     │ 3. 构造SOLICIT消息       │
              │     │ 4. 生成随机IPv6源地址     │
              │     │ 5. 构造RelayMsg与选项    │
              │     │ 6. 序列化并发送UDP包     │
              │     └──────────┬──────────────────┘
              │                │
              │     ┌──────────▼──────────────────┐
              │     │ 增加sent计数             │
              │     └──────────┬──────────────────┘
              │                │
              └────────────────┬─────────────────┐
                               │                 │
                ┌──────────────▼──────────────┐  │
                │ 检查: sent >= count?      │  │
                │ (count > 0 && count reached)│  │
                └──────────┬──────────────────┘  │
                           │ Yes               │
                ┌──────────▼──────────────────┐ │
                │ 日志: "Stress test       │ │
                │       finished. Sent:    │ │
                │       %d packets"         │ │
                │ 删除计时器               │ │
                └──────────────────────────┘ │
                                              │
                                    ┌─────────▼────────────┐
                                    │ 等待下一个10ms周期 │
                                    └─────────────────────┘
```

---

## 九、关键特性总结

| 特性 | 说明 |
|------|------|
| **压力测试包速率** | 可配置(packets per second) |
| **压力测试持续时间** | 可无限发送或设定包数 |
| **SOLICIT消息** | 包含Client ID和IA_NA选项 |
| **MAC地址生成** | 随机生成，设置为locally administered + unicast |
| **IPv6源地址** | 随机生成link-local地址 |
| **Relay包装** | 使用RelayMsg类封装SOLICIT消息 |
| **Option 79支持** | Client Link-Layer Address选项 |
| **Option 18支持** | Interface ID选项 |
| **多服务器支持** | 向所有配置的DHCP服务器发送 |
| **定时精度** | 10ms周期定时器 |
| **计数器集成** | SOLICIT计数存储在STATE_DB |

---

## 十、相关文件位置

- [generate_fake_solicit 函数](src/relay.cpp#L1240)
- [stress_test_callback 函数](src/relay.cpp#L1280)
- [loop_relay 函数](src/relay.cpp#L1377)
- [relay_config 结构](src/relay.h#L64)
- [DHCPv6消息类型定义](src/relay.h#L43)
- [mock测试SOLICIT包](test/mock_relay.cpp#L691)
- [计数器测试](test/mock_relay.cpp#L320)

