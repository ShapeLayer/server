# Part A-1. TCP/IP 커널 동작 원리

게임 서버에서 네트워크 프로그래밍은 단순한 `send()`와 `recv()` 함수 호출이 아니다. 유저 공간(User Space)에서 호출한 소켓 API가 운영체제 커널(Kernel Space)의 TCP/IP 스택에서 어떻게 처리되는지 이해하지 못하면, 대규모 트래픽 환경에서 포트 고갈, 지연 시간(Latency) 급증, 패킷 깨짐, 메모리 부족 등의 치명적인 문제를 겪게 된다.

본 문서에서는 커널 내부의 연결 상태 전이, 송수신 버퍼와 흐름/혼잡 제어, 그리고 스트림 프로토콜의 특성 및 직렬화 기법을 다룬다.

---

## 1. 연결 관리 (Connection Management)

TCP는 연결 지향(Connection-oriented) 프로토콜로, 양단 간의 논리적 연결(Session)을 수립하고 해제하기 위해 상태 머신(Finite State Machine)을 유지한다.

### 1.1 3-Way Handshake와 커널 큐(Queue)

클라이언트가 `connect()`를 호출하고 서버가 `accept()`로 연결을 받아들이기까지 커널 내부에서는 두 개의 큐가 동작한다.

```text
Client                                   Server (Kernel)
  |                                             |
  | --- (1) SYN (seq = x) --------------------> | [SYN Queue (Incomplete Queue)에 엔트리 생성]
  |                                             | 상태: SYN_RCVD
  | <-- (2) SYN + ACK (seq = y, ack = x + 1) -- |
  |                                             |
  | --- (3) ACK (ack = y + 1) ----------------> | [SYN Queue에서 제거 -> Accept Queue로 이동]
  | 상태: ESTABLISHED                           | 상태: ESTABLISHED
  |                                             |
  |                                             | <--- 유저 프로세스가 accept() 호출 시
  |                                             |      Accept Queue에서 소켓 디스크립터 반환
```

1. **`SYN_RCVD` 상태와 SYN Queue**:
   - 서버가 `SYN` 패킷을 수신하면 커널은 반쪽짜리 연결 정보를 **SYN Queue(Incomplete Connection Queue)**에 넣고 `SYN+ACK`를 응답한다.
   - 만약 클라이언트가 악의적으로 `ACK`를 보내지 않고 `SYN`만 대량으로 보내면(**SYN Flooding 공격**) SYN Queue가 가득 차서 정상적인 접속을 받을 수 없게 된다. 현대 OS 커널은 이를 방어하기 위해 큐가 가득 차면 상태 정보를 저장하지 않고 암호학적 해시값을 Sequence Number에 인코딩하는 **SYN Cookies** 기법을 사용한다.
2. **`ESTABLISHED` 상태와 Accept Queue**:
   - 클라이언트의 마지막 `ACK`가 도착하면 커널은 연결을 완료된 것으로 간주하고 **Accept Queue(Completed Connection Queue)**로 옮긴다.
   - `listen(fd, backlog)` 함수의 `backlog` 인자는 이 Accept Queue의 최대 크기를 결정한다(Linux 기준 `/proc/sys/net/core/somaxconn` 값에 의해 상한선이 제한됨).
   - **주의점**: `accept()` 시스템 콜은 3-Way Handshake에 관여하지 않는다. 이미 커널이 Handshake를 끝내놓고 Accept Queue에 넣어둔 연결을 유저 공간으로 꺼내어 새 소켓 파일 디스크립터(FD)를 할당받을 뿐이다. 서버 메인 루프가 블로킹되어 `accept()`를 제때 호출하지 못하면 Accept Queue가 가득 차서 신규 유저의 접속이 거부(`ECONNREFUSED` 또는 타임아웃)된다.

---

### 1.2 4-Way Handshake와 주요 상태 분석

TCP 연결은 전이중(Full-Duplex) 통신이므로 양쪽 방향의 스트림을 각각 닫아야 한다. 먼저 `close()` 또는 `closesocket()`을 호출하는 쪽이 **Active Closer**, 상대방의 종료 요청을 받는 쪽이 **Passive Closer**가 된다.

```text
Active Closer (예: Server)               Passive Closer (예: Client)
  |                                             |
  | close() 호출                                |
  | --- (1) FIN ------------------------------> |
  | 상태: FIN_WAIT_1                            | 상태: CLOSE_WAIT (recv()가 0을 반환함)
  |                                             |
  | <-- (2) ACK ------------------------------- |
  | 상태: FIN_WAIT_2                            | (남은 데이터 전송 가능)
  |                                             |
  |                                             | close() 호출
  | <-- (3) FIN ------------------------------- | 상태: LAST_ACK
  | 상태: TIME_WAIT                             |
  | --- (4) ACK ------------------------------> |
  | (2 * MSL 대기 후 CLOSED)                    | 상태: CLOSED
```

#### ① `TIME_WAIT` 상태 (Active Closer 측에서 발생)
- **발생 시점**: 마지막 `FIN`을 수신하고 `ACK`를 보낸 직후 진입하며, **2 * MSL (Maximum Segment Lifetime, 보통 1~4분)** 동안 유지된다.
- **존재 이유**:
  1. **지연된 이전 패킷으로 인한 오류 방지**: 네트워크 상에서 길을 잃고 떠돌던 이전 연결의 패킷(Wandering Duplicate Packet)이 우연히 동일한 `(송신 IP, 송신 Port, 수신 IP, 수신 Port)` 4-Tuple로 새로 맺어진 세션에 도착해 데이터 무결성을 해치는 것을 막는다.
  2. **마지막 `ACK` 유실 대비**: Active Closer가 보낸 마지막(4번째) `ACK`가 유실되면, Passive Closer는 `LAST_ACK` 상태에서 타임아웃이 발생하여 `FIN`을 재전송한다. 만약 Active Closer가 즉시 `CLOSED` 되어버리면 재전송된 `FIN`에 대해 `RST(Reset)`를 응답하게 되어 상대방이 비정상 종료로 오인하게 된다.
- **게임 서버에서의 문제점**:
  - 서버가 먼저 연결을 끊는 구조(예: 킥 처리, 프로세스 재시작)일 때 서버 측에 `TIME_WAIT` 소켓이 남는다.
  - 서버 프로세스가 크래시 후 즉시 재시작될 때, 이전 프로세스가 바인딩했던 포트가 `TIME_WAIT` 상태에 묶여 있으면 `bind()`가 `EADDRINUSE` 에러를 뱉으며 서버가 켜지지 않는다.
  - 서버가 DB나 외부 인증 서버로 직접 `connect()`를 맺는 클라이언트 역할을 할 경우, 짧은 주기로 연결/해제를 반복하면 가용 로컬 포트(Ephemeral Port, 보통 32768~60999)가 모두 `TIME_WAIT`에 빠져 새로운 연결을 맺지 못하는 **포트 고갈(Port Exhaustion)** 현상이 발생한다. (이 때문에 DB나 웹 통신 시 반드시 **Connection Pool**을 유지해야 한다.)

#### ② `CLOSE_WAIT` 상태 (Passive Closer 측에서 발생)
- **발생 시점**: 상대방으로부터 `FIN`을 받아 커널이 `ACK`는 보냈으나, **내 애플리케이션에서 아직 해당 소켓에 대해 `close()` / `closesocket()`을 호출하지 않은 상태**다.
- **게임 서버에서의 문제점**:
  - `TIME_WAIT`가 네트워크 프로토콜 설계상 정상적인 대기 상태라면, **`CLOSE_WAIT`가 누적되는 것은 100% 서버 애플리케이션의 버그(Resource Leak)**다.
  - 예: 비동기 I/O에서 `recv()` 완료 통지로 `0 바이트`(EOF)를 전달받았음에도 불구하고, 세션 참조 카운트 버그나 데드락으로 인해 소켓 핸들을 닫는 로직(`closesocket`)이 호출되지 않으면 소켓이 영원히 `CLOSE_WAIT` 상태로 남아 파일 디스크립터와 커널 메모리를 고갈시킨다.

---

### 1.3 소켓 옵션을 통한 연결 제어

#### ① `SO_REUSEADDR`
`TIME_WAIT` 상태에 있는 포트에 즉시 다시 `bind()`할 수 있도록 허용하는 옵션이다. 서버 리스닝 소켓 초기화 시 `bind()` 호출 전에 설정한다.

```cpp
int opt = 1;
setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
```
> **Windows 주의사항**: Linux에서는 `SO_REUSEADDR`가 `TIME_WAIT` 상태의 포트만 재사용하게 해주지만, Windows의 `SO_REUSEADDR`는 이미 다른 프로세스가 정상적으로 `LISTEN` 중인 포트까지 가로채서 바인딩할 수 있게 허용하는 보안/동작상의 문제가 있다. 따라서 Windows 서버에서는 필요 시 `SO_EXCLUSIVEADDRUSE`를 함께 고려해야 한다.

#### ② `SO_LINGER`
`close()` / `closesocket()` 호출 시 커널 송신 버퍼에 아직 전송되지 않은 데이터가 남아있거나 Handshake를 어떻게 처리할지 결정한다.

```cpp
struct linger {
    u_short l_onoff;  // 0 = 비활성화, 1 = 활성화
    u_short l_linger; // 대기 시간 (초 단위)
};
```

3가지 조합에 따른 동작 차이는 다음과 같다:

| `l_onoff` | `l_linger` | 종료 방식 | 커널 동작 및 특징 |
| :---: | :---: | :--- | :--- |
| **0** (기본값) | 무관 | 우아한 종료 (Graceful Close) | `close()`는 즉시 반환된다. 커널은 송신 버퍼의 남은 데이터를 백그라운드에서 모두 전송한 뒤 정상적인 4-Way Handshake(`FIN`)를 진행하며, 결과적으로 `TIME_WAIT`가 남는다. |
| **1** | **> 0** | 블로킹 우아한 종료 | 송신 버퍼의 데이터를 모두 보내고 상대방의 `ACK`를 받을 때까지(또는 `l_linger` 초가 지날 때까지) `close()` 호출 스레드가 블로킹된다. 시간 내에 못 보내면 `RST`를 보낸다. 멀티스레드 게임 서버에서는 스레드가 멈추므로 절대 사용하지 않는다. |
| **1** | **0** | 강제 종료 (Abortive Close) | `close()` 즉시 커널 송신 버퍼의 미전송 데이터를 전부 폐기하고 상대방에게 **`RST(Reset)` 패킷**을 쏜다. 4-Way Handshake를 생략하고 소켓이 즉시 `CLOSED` 상태로 파괴되므로 **`TIME_WAIT`가 남지 않는다.** |

- **실무 활용**: 비정상적인 패킷을 보내는 핵 유저를 강제 퇴장시키거나, 더미 클라이언트 Chaos 테스트에서 서버에 `TIME_WAIT` 부담 없이 수천 개의 연결을 강제 절단할 때 `l_onoff = 1, l_linger = 0`을 사용한다.

---

## 2. 흐름 제어(Flow Control) & 혼잡 제어(Congestion Control)

### 2.1 송수신 커널 버퍼(Send / Recv Buffer)의 동작 원리

애플리케이션에서 호출하는 `send()`와 `recv()`는 네트워크 카드(NIC)로 직접 패킷을 쏘거나 받는 함수가 아니다. 정확히는 **유저 공간의 버퍼와 OS 커널의 소켓 버퍼 간에 메모리를 복사(Copy)하는 함수**다.

```text
[User Space]                        [Kernel Space]                       [Hardware]
+---------------+   send() 복사     +--------------------+   TCP 세그먼트  +-------+
| App SendBuf   | ----------------> | Socket Send Buffer | --------------> |       |
+---------------+                   +--------------------+                 |  NIC  |
+---------------+   recv() 복사     +--------------------+   패킷 수신     |       |
| App RecvBuf   | <---------------- | Socket Recv Buffer | <-------------- |       |
+---------------+                   +--------------------+                 +-------+
```

1. **`send()` 호출 시**:
   - 유저 버퍼의 데이터를 커널의 `Socket Send Buffer`(`SO_SNDBUF`)에 복사하고 즉시 반환한다.
   - 실제 네트워크 전송은 커널의 TCP 스택이 상대방의 수신 윈도우 크기와 네트워크 혼잡도를 계산하여 비동기적으로 수행한다.
   - 상대방으로부터 `ACK`가 도착하기 전까지 커널은 재전송(Retransmission)을 대비해 `Socket Send Buffer`에서 데이터를 지우지 않고 보관한다.
   - 만약 상대방이 `recv()`를 안 해서(또는 네트워크가 막혀서) `ACK`가 안 오면, 서버의 `Socket Send Buffer`가 가득 차게 된다. 이때 Blocking 소켓이면 `send()`가 블로킹되고, Non-blocking 소켓이면 `EWOULDBLOCK` 에러가 반환되거나 요청한 크기보다 적은 바이트(Partial Write)만 복사된다.
2. **`recv()` 호출 시**:
   - NIC를 통해 들어온 패킷은 인터럽트(또는 NAPI 폴링)를 거쳐 커널의 `Socket Recv Buffer`(`SO_RCVBUF`)에 차곡차곡 쌓인다.
   - 유저가 `recv()`를 호출하면 커널은 `Socket Recv Buffer`에 쌓여 있는 바이트를 유저 버퍼로 복사해 주고, 복사해 간 만큼 버퍼 공간을 비운다.

---

### 2.2 흐름 제어 (Flow Control)와 Sliding Window

송신 측이 수신 측의 처리 속도보다 빠르게 데이터를 보내면 수신 측 커널 버퍼(`SO_RCVBUF`)가 넘쳐 패킷이 유실된다. 이를 막기 위해 **수신 측이 자신의 남은 커널 버퍼 크기를 송신 측에 매 패킷마다 알려주어 전송 속도를 제한**하는 것이 흐름 제어다.

- **RWND (Receive Window)**: TCP 헤더의 `Window Size` 필드에 담기는 값으로, 수신 측의 `Socket Recv Buffer` 여유 공간 크기다.
- **Sliding Window**: 송신 측은 상대방의 `ACK`를 받지 않고도 `RWND` 크기만큼은 연속해서 패킷을 보낼 수 있다. `ACK`를 받으면 창문(Window)을 오른쪽으로 밀어(Slide) 다음 데이터를 전송한다.
- **Zero Window와 Persist Timer**:
  - 만약 게임 서버가 무거운 로직(예: 동기 DB 쿼리)을 처리하느라 `recv()`를 호출하지 못하면, 서버의 `Socket Recv Buffer`가 가득 차서 클라이언트에게 `Window Size = 0`(**Zero Window**)을 통보한다.
  - 클라이언트는 즉시 전송을 중단한다. 이후 서버가 `recv()`를 호출해 버퍼가 비워지면 서버는 윈도우 업데이트 패킷을 보내지만, 이 패킷이 유실되면 양쪽이 서로 영원히 기다리는 데드락에 빠질 수 있다. 이를 방지하기 위해 송신 측은 주기적으로 1바이트짜리 **Zero Window Probe(ZWP)** 패킷을 보내 수신 버퍼가 비었는지 확인한다.

---

### 2.3 혼잡 제어 (Congestion Control)

흐름 제어가 '수신자(End-Host)의 버퍼 상태'를 배려하는 것이라면, 혼잡 제어는 송수신자 사이의 **'네트워크 라우터/망의 상태'**를 배려하는 메커니즘이다.

- 송신 측은 네트워크의 혼잡도를 추측하기 위해 내부적으로 **CWND (Congestion Window)**라는 변수를 관리한다.
- **실제 전송 가능한 최대 데이터양** = $\min(\text{RWND}, \text{CWND})$
- **핵심 동작 과정**:
  1. **Slow Start**: 연결 직후에는 네트워크 대역폭을 모르므로 `CWND`를 작게(보통 10 MSS) 시작해 `ACK`를 받을 때마다 지수적으로(2배씩) 늘린다.
  2. **Congestion Avoidance**: 임계점(`ssthresh`)에 도달하면 선형적으로 조금씩 늘린다.
  3. **패킷 유실 감지 시**: 타임아웃이나 중복 ACK(3 Duplicate ACKs)가 발생하면 경로상의 라우터 큐가 터진 것으로 간주하고 `CWND`를 급격히 줄인다(Reno, CUBIC 알고리즘 등). 최근에는 패킷 유실이 아닌 대역폭과 RTT 변화를 측정해 전송량을 조절하는 Google의 **BBR** 알고리즘도 널리 쓰인다.
- **게임 서버 관점의 시사점**: TCP는 패킷이 단 1개만 유실되어도(Packet Loss) 재전송이 완료될 때까지 뒤이어 도착한 정상 패킷들을 유저에게 올려주지 않고 커널 버퍼에 묶어두며(**Head-of-Line Blocking**), 동시에 혼잡 제어로 인해 전송 속도(`CWND`)까지 깎아버린다. 반응 속도가 극도로 중요한 FPS나 격투 게임에서 이동/조준 동기화에 UDP(또는 RUDP)를 혼용하는 이유가 바로 이 때문이다.

---

### 2.4 Nagle 알고리즘과 `TCP_NODELAY`

#### ① Nagle 알고리즘의 원리
1984년 John Nagle이 제안한 알고리즘으로, **작은 크기의 패킷(Tinygram)이 네트워크에 범람하는 것을 막기 위해 고안**되었다.
- 만약 1바이트의 데이터를 보내기 위해 TCP(20B) + IP(20B) 헤더를 붙이면 총 41바이트가 전송되어 오버헤드가 4000%에 달한다.
- **동작 규칙**:
  1. 보낼 데이터가 **MSS(Maximum Segment Size, 보통 1460 바이트)** 이상이면 즉시 전송한다.
  2. 데이터가 MSS보다 작더라도, **이전에 보낸 모든 패킷에 대한 `ACK`를 다 받은 상태(In-flight 패킷이 없는 상태)**라면 첫 번째 작은 패킷은 즉시 전송한다.
  3. 아직 앞서 보낸 패킷의 `ACK`가 돌아오지 않았다면, 새로 요청된 작은 데이터들은 바로 보내지 않고 **커널 송신 버퍼에 모은다(Coalescing)**.
  4. 이전 패킷의 `ACK`가 도착하거나, 모인 데이터가 MSS에 도달하면 그때 한 번에 묶어서 전송한다.

```text
[Nagle ON (기본값)]
App send("A") -> 즉시 전송 (In-flight 패킷 없음)
App send("B") -> 대기 (이전 "A"의 ACK 대기 중)
App send("C") -> 대기 ("B"와 함께 커널 버퍼에 병합: "BC")
... (RTT 시간 경과 후 "A"에 대한 ACK 도착) ...
Kernel -> 합쳐진 "BC" 패킷 전송! (최소 1 RTT 만큼 전송 지연 발생)
```

#### ② Delayed ACK와의 치명적인 상호작용
수신 측 TCP 역시 패킷을 받을 때마다 즉시 빈 `ACK` 패킷을 보내는 것이 아까워서, **"조금 기다렸다가(보통 40ms~200ms) 내가 보낼 데이터가 생기면 그때 ACK를 얹어서(Piggyback) 같이 보내자"**라는 **Delayed ACK** 기법을 사용한다.

이 두 기능이 만나면 실시간 게임에서 재앙이 발생한다:
- **서버(Nagle ON)**: "클라이언트가 이전 패킷의 `ACK`를 줄 때까지 다음 이동 패킷 전송을 보류해야지."
- **클라이언트(Delayed ACK ON)**: "서버가 추가 패킷을 주거나 내가 보낼 패킷이 생길 때까지 `ACK` 전송을 40ms 동안 미뤄야지."
- 결과적으로 양쪽이 서로를 기다리느라 아무 일도 하지 않는 **40ms~200ms의 강제 렉(Latency Spike)**이 발생한다.

#### ③ 해결책: `TCP_NODELAY` 활성화
실시간성이 생명인 게임 서버에서는 대역폭을 약간 손해 보더라도 지연 시간(Latency)을 줄이는 것이 훨씬 중요하다. 따라서 `TCP_NODELAY` 옵션을 켜서 **Nagle 알고리즘을 비활성화**한다.

```cpp
int flag = 1;
setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
```
> **참고**: `TCP_NODELAY`를 켜서 커널의 자동 병합을 끄는 대신, 게임 서버 엔진 단에서(유저 공간에서) `SendQueue`나 `SendBufferChunk`를 두어 한 틱(Tick) 동안 발생한 패킷들을 직접 모아서 한 번의 `WSASend`/`writev`로 밀어 넣는 방식으로 대역폭과 반응성을 동시에 잡는다.

---

## 3. 스트림(Stream) 특성과 패킷 직렬화

### 3.1 TCP의 경계 없음(Boundaryless) 현상

UDP가 보낸 단위 그대로 도착하는 데이터그램(Datagram) 방식인 것과 달리, **TCP는 바이트 스트림(Byte Stream) 프로토콜**이다. 즉, 커널은 애플리케이션이 `send()`를 몇 번에 나누어 호출했는지 전혀 기억하지 않으며, 오직 연속된 바이트의 물줄기로만 취급한다.

이로 인해 수신 측 `recv()`에서는 두 가지 현상이 일상적으로 발생한다.

#### ① 패킷 분할 (Fragmentation / Partial Read)
송신 측이 100바이트짜리 공격 패킷 1개를 한 번의 `send(100)`로 보냈더라도, 네트워크 경로의 MTU 제한, 슬라이딩 윈도우 잔여량 등에 의해 수신 측 `recv()`에서는 **60바이트와 40바이트로 두 번에 나뉘어 읽힐 수 있다.** 심지어 극단적인 경우 패킷 헤더(4바이트)조차 2바이트씩 쪼개져 도착할 수 있다.

#### ② 패킷 병합 (Coalescing / Sticky Packet)
송신 측이 30바이트짜리 이동 패킷 3개를 `send(30)`, `send(30)`, `send(30)`으로 연속 호출했더라도, 커널 송신 버퍼에서 합쳐지거나 수신 측이 `recv()`를 호출하기 전에 커널 수신 버퍼에 3개가 모두 도착해버리면, **한 번의 `recv()` 호출에 90바이트가 통째로 읽힌다.**

```text
송신 측 send() 호출:   [ 패킷 1 (30B) ] [ 패킷 2 (30B) ] [      패킷 3 (100B)      ]
                                    | (TCP Byte Stream)
                                    v
수신 측 recv() 결과:   [     1차 recv: 60B (패킷1 + 패킷2)     ] [ 2차 recv: 40B ] [ 3차 recv: 60B ]
                                                                 \_______패킷 3이 쪼개짐_______/
```

---

### 3.2 패킷 헤더 프레이밍 (Packet Framing)

바이트 스트림에서 어디서부터 어디까지가 의미 있는 하나의 메시지(패킷)인지 경계를 구분하는 작업을 **프레이밍(Framing)**이라고 한다.

HTTP/1.1 같은 텍스트 프로토콜은 `\r\n\r\n` 같은 구분자(Delimiter)를 사용하지만, 바이너리 데이터를 다루는 게임 서버에서는 구분자와 실제 데이터 바이트가 겹칠 수 있어(Byte Stuffing 필요) 비효율적이다. 따라서 게임 서버는 거의 예외 없이 **고정 크기 헤더(Length + Packet ID) + 가변 페이로드(Payload)** 방식을 사용한다.

#### 패킷 메모리 레이아웃
```text
+-------------------+-------------------+---------------------------------------+
|   size (2 Byte)   |    id (2 Byte)    |           payload (N Byte)            |
|  (전체 패킷 크기)   |   (패킷 종류 식별)  |     (직렬화된 실제 데이터: Protobuf 등)    |
+-------------------+-------------------+---------------------------------------+
|<---------- PacketHeader (4B) -------->|<-------- size - sizeof(Header) ------>|
|<----------------------------------- size ------------------------------------>|
```

#### C++ 구현 구조체 및 파싱 로직

네트워크를 통해 구조체를 직접 읽고 쓸 때는 컴파일러가 임의로 패딩 바이트(Padding)를 넣지 못하도록 `#pragma pack(push, 1)`을 지정해야 한다.

```cpp
#pragma pack(push, 1)
struct PacketHeader
{
    uint16_t size; // 헤더 크기를 포함한 전체 패킷의 바이트 크기
    uint16_t id;   // 패킷 프로토콜 ID (예: 1001 = C_LOGIN, 1002 = C_MOVE)
};
#pragma pack(pop)

// 수신 링버퍼(RecvBuffer)에 데이터가 들어왔을 때 호출되는 파싱 루프
int32_t OnRecv(BYTE* buffer, int32_t len)
{
    int32_t processLen = 0;

    while (true)
    {
        int32_t dataSize = len - processLen;

        // 1. 최소한 헤더(4바이트)조차 아직 다 도착하지 않았다면 다음 recv를 기다린다.
        if (dataSize < sizeof(PacketHeader))
            break;

        // 2. 헤더를 읽어서 전체 패킷 크기(size)를 확인한다.
        PacketHeader* header = reinterpret_cast<PacketHeader*>(&buffer[processLen]);

        // [보안 검증] 악의적인 클라이언트가 size를 0으로 보내 무한 루프를 유도하거나,
        // 버퍼 최대 크기보다 큰 값을 보내는 경우 즉시 세션을 끊어야 한다.
        if (header->size < sizeof(PacketHeader) || header->size > MAX_PACKET_SIZE)
        {
            Disconnect("Invalid Packet Size");
            return -1;
        }

        // 3. 헤더에 적힌 전체 패킷 크기만큼 데이터가 아직 다 모이지 않았다면(Fragmentation) 루프 탈출
        if (dataSize < header->size)
            break;

        // 4. 온전한 패킷 1개가 조립되었으므로 컨텐츠 핸들러로 전달한다.
        // 만약 여러 패킷이 뭉쳐서(Coalescing) 들어왔다면 while 루프를 돌며 연달아 처리된다.
        OnRecvPacket(&buffer[processLen], header->size);

        processLen += header->size;
    }

    // 이번 호출에서 성공적으로 처리한 총 바이트 수를 반환하여 RecvBuffer의 ReadPos를 전진시킨다.
    return processLen;
}
```

---

### 3.3 패킷 직렬화 (Serialization)

객체의 메모리 데이터를 네트워크로 전송 가능한 일련의 바이트 배열로 변환하는 것을 **직렬화(Serialization / Marshaling)**, 반대로 바이트 배열에서 객체로 복원하는 것을 **역직렬화(Deserialization / Unmarshaling)**라고 한다.

#### 직렬화 시 고려해야 할 기초 요소
1. **포인터 및 참조 전송 불가**: 메모리 주소값(`0x7ff...`)은 다른 컴퓨터에서 아무 의미가 없으므로, `std::string`이나 `std::vector` 같은 동적 컨테이너는 `[길이(Length)][실제 원소 데이터...]` 형태로 풀어서(Flatten) 기록해야 한다.
2. **바이트 오더 (Endianness)**: x86/x64 및 현대 모바일 ARM 아키텍처는 모두 **Little Endian**을 사용하므로, 클라이언트와 서버가 모두 현대 환경이라면 별도의 엔디안 변환(`htonl` 등) 없이 Little Endian 기준으로 통일하여 CPU 연산을 아끼는 것이 일반적이다.

#### 주요 직렬화 방식 비교

| 구분 | Custom Binary (수동/자체 IDL) | Google Protocol Buffers (Protobuf) | Google FlatBuffers |
| :--- | :--- | :--- | :--- |
| **원리** | 버퍼에 직접 변수를 순서대로 Read/Write 하는 코드를 작성하거나 코드 생성기로 생성 | `.proto` 스키마 기반으로 Tag-Length-Value 인코딩 및 가변 길이 정수(**Varint**) 압축 수행 | 데이터를 직렬화할 때 내부 오프셋 테이블(vtable)을 함께 기록하여 **역직렬화 과정 없이** 버퍼에서 바로 읽음 |
| **장점** | - 오버헤드가 거의 없음<br>- 외부 라이브러리 의존성 없음 | - **강력한 하위/상위 호환성**(필드 추가/삭제에 유연)<br>- Varint 압축으로 패킷 크기가 작음<br>- 레퍼런스와 생태계가 가장 풍부함 | - **Zero-copy 역직렬화**: 수신 버퍼 포인터만 캐스팅하면 즉시 필드 접근 가능<br>- 객체 생성 및 힙 할당(`new`) 비용이 0 |
| **단점** | - 프로토콜 변경 시 구버전 클라이언트와 호환성 유지가 까다로움<br>- 자체 파서/생성기 유지보수 필요 | - 역직렬화 시 임시 객체 생성 및 `std::string` 등 동적 메모리 할당 비용이 발생함 | - Varint 압축을 안 하고 오프셋 테이블이 들어가므로 패킷 크기가 Protobuf보다 큼<br>- API 사용법(Builder 패턴)이 다소 번거로움 |
| **주 사용처** | 자체 엔진을 보유한 전통적인 MMORPG 코어 | 대다수의 현대 게임 서버(로비, 채팅, 인게임 일반 패킷, 서버 간 통신) | 초당 수십만 건의 이동/물리 패킷을 브로드캐스팅해야 하는 극한의 실시간 액션/배틀로얄 서버 |

#### 실무 적용 팁 (Protobuf + Arena Allocation)
Protobuf의 가장 큰 단점은 패킷을 역직렬화(`ParseFromArray`)할 때 내부의 문자열이나 반복 필드(`repeated`) 때문에 힙 할당(`malloc`/`new`)이 빈번하게 일어난다는 점이다. 실무에서는 이를 완화하기 위해 패킷 처리 스레드의 스택이나 TLS(Thread Local Storage) 메모리 블록을 재사용하거나, Protobuf가 기본 제공하는 **`google::protobuf::Arena`** 기능을 활성화하여 동적 할당 오버헤드를 제거한다.

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **`TIME_WAIT` vs `CLOSE_WAIT`**: `TIME_WAIT`는 먼저 끊은 쪽(Active Closer)에서 패킷 무결성과 마지막 ACK 유실 방지를 위해 기다리는 정상 상태이며, `CLOSE_WAIT`는 상대가 끊었는데 내가 `close()`를 안 호출한 애플리케이션 버그 상태다.
2. **`TCP_NODELAY`**: Nagle 알고리즘과 Delayed ACK의 조합으로 인한 수십 ms의 지연을 막기 위해 실시간 게임 서버에서는 필수적으로 켠다.
3. **패킷 프레이밍**: TCP는 스트림 프로토콜이라 패킷 경계가 없으므로, 반드시 `[Size][ID]` 헤더를 두어 분할(Fragmentation)과 병합(Coalescing)을 모두 처리하는 파싱 루프를 구현해야 한다.
