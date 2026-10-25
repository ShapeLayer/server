# 핵심 이론 학습 로드맵 (네트워크 & 동시성 중심)

## Part A. 네트워크 프로그래밍 심화
1. **[TCP/IP 커널 동작 원리](./A1_tcp_ip_kernel.md)**
   - **연결 관리**: 3-Way / 4-Way Handshake 과정과 각 상태(`SYN_RCVD`, `ESTABLISHED`, `TIME_WAIT`, `CLOSE_WAIT`)의 발생 원인 및 소켓 옵션(`SO_REUSEADDR`, `SO_LINGER`)을 통한 제어
   - **흐름 제어 & 혼잡 제어**: 송수신 커널 버퍼(Send/Recv Buffer)의 동작, Sliding Window, Nagle 알고리즘(`TCP_NODELAY` 옵션으로 비활성화하는 이유)
   - **스트림(Stream) 특성과 패킷 직렬화**: TCP의 경계 없음(Fragmentation / Coalescing) 현상과 이를 해결하기 위한 **패킷 헤더(Length + Packet ID) 프레이밍**, 직렬화(Protobuf, FlatBuffers, Custom Binary)
2. **[소켓 I/O 모델의 진화](./A2_socket_io_models.md)**
   - **Blocking vs Non-Blocking I/O**: 시스템 콜 호출 시 스레드 블로킹 여부와 `EWOULDBLOCK` / `WSAEWOULDBLOCK` 처리
   - **Synchronous vs Asynchronous I/O**: I/O 완료 통지 주체와 데이터 복사 주체의 차이 (Reactor 패턴 vs Proactor 패턴)
   - **Multiplexing 기초**: `select`, `poll`의 한계(FD 개수 제한, $O(N)$ 순회 비용, 유저-커널 메모리 복사)
3. **[고성능 OS 비동기/멀티플렉싱 I/O 엔진](./A3_async_io_engines.md)**
   - **Windows IOCP (I/O Completion Port)** *(국내 게임사 C++ 서버 주력)*:
     - `Overlapped I/O` 구조체와 비동기 송수신(`WSASend`, `WSARecv`)
     - 커널 I/O 큐와 Completion Port 스레드 풀링 메커니즘(Running Thread 수 제어와 Context Switching 최소화 원리)
     - Page Locking 한계와 **Zero-Byte Receive** 기법
   - **Linux epoll / io_uring** *(크로스 플랫폼 및 글로벌/모바일 서버)*:
     - `epoll`의 Red-Black Tree + Ready List 구조($O(1)$ 감지)
     - **Level-Triggered(LT) vs Edge-Triggered(ET)** 차이점과 ET 모드에서 Non-blocking 루프가 필수인 이유

---

## Part B. 동시성(Concurrency) & 멀티스레드 프로그래밍
1. **[CPU 아키텍처와 메모리 계층](./B1_cpu_and_memory.md)**
   - **Context Switching 비용**: 레지스터 백업, TLB 플러시, CPU 캐시 오염
   - **캐시 일관성(Cache Coherency)**: MESI 프로토콜, **False Sharing(거짓 공유)** 현상과 `alignas(64)`(캐시 라인 패딩)를 통한 회피
   - **Memory Visibility & Reordering**: 컴파일러/CPU의 명령어 재배치와 `std::memory_order`(relaxed, acquire, release, seq_cst)의 의미
2. **[동기화 객체와 데드락(Deadlock)](./B2_synchronization_and_deadlock.md)**
   - **User-mode vs Kernel-mode 동기화**: SpinLock, `CRITICAL_SECTION` / `futex`, `Mutex`, `Semaphore`, `SRWLock`(Read-Write Lock)의 내부 동작과 전환 비용
   - **Condition Variable**: Spurious Wakeup(가짜 기상) 현상과 이벤트 대기 패턴
   - **데드락(Deadlock) 탐지 및 회피**: 4대 발생 조건, Lock Hierarchy(락 순서 강제) 설계, 그래프 탐색을 통한 런타임 데드락 탐지기 구현
3. **[게임 서버 동시성 아키텍처 패턴](./B3_concurrency_patterns.md)**
   - **Lock 기반 공유 메모리 방식의 한계**: 세밀한 락(Fine-grained Lock)에서 발생하는 데드락과 경합(Contention) 비용
   - **Job Queue (Actor 모델 / Command 패턴)**:
     - 룸(Room)이나 존(Zone) 단위로 실행 큐(Job Queue)를 두고, 워커 스레드가 큐에서 일감을 하나씩 꺼내 **순차 실행(Single-threaded execution guarantee)**하도록 만들어 콘텐츠 코드에서 Lock을 제거하는 기법
   - **Lock-Free 자료구조 기초**:
     - `CAS (Compare-And-Swap)` 원자적 연산의 원리
     - **ABA 문제** 발생 시나리오와 해결책(Double-word CAS / Stamp, Hazard Pointer)
     - Lock-Free Stack / Queue (MPSC, MPMC) 구조 이해

---

## Part C. 서버 메모리 관리 및 안정성
1. **[커스텀 메모리 할당자](./C1_custom_memory_allocators.md)**:
   - 빈번한 `new`/`delete`로 인한 힙 파편화(Fragmentation)와 멀티스레드 힙 락(Heap Lock) 경합 해결
   - **Object Pool**, **Memory Pool(Slab/Size-class 기반)**, **TLS(Thread Local Storage) 메모리 풀** 구현
2. **[네트워크 버퍼 설계](./C2_network_buffer_design.md)**:
   - **Ring Buffer(Circular Buffer)**: TCP 수신 스트림 조립 시 메모리 이동(`memmove`) 최소화
   - **SendBuffer Pooling & Gather I/O**: 브로드캐스팅 시 패킷 복사 비용을 줄이기 위한 참조 카운팅(`shared_ptr` 기반 청크) 및 `WSASend` 다중 버퍼 전송

---
---

# C++ 게임 서버 & 더미 CLI 클라이언트 실습 설계

단순 에코 서버에서 시작해 단계적으로 기능을 얹어 **하나의 코어 네트워크 라이브러리 + 게임 서버 + 더미 클라이언트** 세트로 완성하는 5단계 실습 구조입니다.

## 전체 프로젝트 디렉터리 구조
```text
GameServerProject/
├── ServerCore/          # 재사용 가능한 네트워크/동시성 정적 라이브러리
│   ├── Network/         # IOCP(또는 epoll) 코어, Session, Listener, RingBuffer, SendBuffer
│   ├── Thread/          # ThreadManager, TLS, DeadlockProfiler, RwLock
│   ├── Memory/          # ObjectPool, MemoryPool, CustomAllocator
│   └── Job/             # Job, JobQueue, GlobalQueue, JobTimer
├── Common/              # 서버-클라이언트 공유 코드
│   └── Protocol/        # 패킷 정의(.proto 또는 IDL) 및 자동 생성된 PacketHandler
├── GameServer/          # 실제 게임 로직 서버
│   ├── Room/            # GameRoom (JobQueue 상속), Sector(시야 처리)
│   ├── Object/          # Player, Monster, Projectile
│   └── DB/              # DBConnectionPool, AsyncDBWorker
└── DummyClient/         # 부하 테스트용 CLI 더미 클라이언트
    ├── Bot/             # ClientSession, BotStateMachine (FSM)
    └── Metrics/         # RTT 측정기, 초당 처리 패킷(TPS) 모니터, CLI 대시보드
```

---

## 단계별 실습 과제 (Step-by-Step)

### Step 1. TCP 스트림 프레이밍 & 링버퍼 에코 서버
- **목표**: TCP 바이트 스트림을 패킷 단위로 쪼개고 조립하는 기초 체력 확보
- **서버 구현**:
  - 비동기 I/O 코어(Windows라면 IOCP, Linux라면 epoll ET) 위에 `Listener`와 `Session` 클래스 구축
  - 수신용 **Ring Buffer** 직접 구현 (ReadPos, WritePos 관리 및 버퍼 래핑 처리)
  - `[uint16 size][uint16 id][payload...]` 헤더 기반 패킷 파싱 루프 작성
- **CLI 더미 클라이언트 구현**:
  - 일부러 패킷을 1바이트씩 쪼개 보내거나(Fragmentation), 패킷 10개를 한 번의 `send`로 뭉쳐 보내는(Coalescing) 악성 테스트 케이스 실행
- **검증 포인트**: 수만 번의 변칙 송수신에도 패킷 오염이나 크래시 없이 정확히 에코되는지 확인

### Step 2. 멀티스레드 환경과 생명주기(Life-cycle) 안전성 확보
- **목표**: 멀티스레드 워커 환경에서 세션이 끊길 때 발생하는 **Use-After-Free(댕글링 포인터)** 완벽 제어
- **서버 구현**:
  - 워커 스레드 N개 가동 및 `SendBuffer` 모음 전송(Scatter-Gather I/O 또는 Send Queue Locking)
  - 비동기 I/O가 걸려 있는 도중 클라이언트가 강제 종료될 때 `Session` 객체가 즉시 삭제되지 않도록 참조 카운트(`shared_ptr` + Overlapped 참조 유지) 설계
  - 메모리 풀(`ObjectPool`) 도입 및 디버그 모드에서 **Stomp Allocator**(페이지 끝부분에 할당해 오버플로우/해제 후 접근 시 즉시 하드웨어 예외 발생) 적용
- **CLI 더미 클라이언트 구현**:
  - 500개의 세션을 띄워 **0.1초마다 무작위로 접속/강제종료(`closesocket` with `SO_LINGER` 0)/패킷 폭격**을 반복하는 Chaos 테스트 모드 구현
- **검증 포인트**: 1시간 이상 Chaos 테스트 구동 시 메모리 누수(Leak) 및 Access Violation 0건 달성

### Step 3. 프로토콜 자동화와 브로드캐스팅 (채팅/로비 서버)
- **목표**: 패킷 직렬화 자동화 및 다대다 브로드캐스팅 시 발생하는 락 경합 체험
- **서버 구현**:
  - Protobuf(또는 FlatBuffers) 연동 및 패킷 ID별 핸들러 함수 포인터 테이블 자동 생성 스크립트 작성
  - `Room` 클래스에 100명 입장 후 채팅/이동 패킷 브로드캐스팅 (1차: `Mutex` 기반 구현)
  - 브로드캐스팅 시 각 세션마다 `SendBuffer`를 복사하지 않고, **하나의 직렬화된 `SendBufferChunk`를 `shared_ptr`로 공유**하여 Zero-copy 브로드캐스팅 구현
- **CLI 더미 클라이언트 구현**:
  - 패킷 상단에 클라이언트 타임스탬프(`uint64 tick`)를 심어 보내고, 돌아온 브로드캐스트 패킷으로 **RTT(Round Trip Time) 평균/P95/P99 지연시간**을 계산해 CLI 화면에 매초 출력

### Step 4. Job Queue(Actor 모델) 도입을 통한 Lock 제거
- **목표**: 게임 로직 스레드에서 락 경합을 없애고 안전한 비동기 실행 흐름 구축
- **서버 구현**:
  - Step 3의 `Room` 내 `Mutex`를 모두 제거하고, `Room`이 `JobQueue`를 상속받도록 리팩터링
  - 패킷 핸들러는 패킷을 파싱한 뒤 `room->PushJob([=](){ room->HandleMove(player, pkt); });` 형태로 일감만 등록하고 즉시 반환
  - `GlobalQueue`와 `JobTimer`(예약 실행 큐, 우선순위 큐 기반)를 구현해 몬스터 AI 틱이나 스킬 쿨타임 같은 주기적 작업을 워커 스레드가 분배 처리하도록 구성
- **검증 포인트**: Step 3(Lock 방식) 대비 동접 1,000명 브로드캐스팅 시 **CPU 사용률 및 P99 RTT 개선 수치 비교 기록** (이 수치가 이력서의 핵심 성과가 됩니다)

### Step 5. 2D 그리드/섹터 기반 시야 처리(AOI) & 더미 봇 시뮬레이션
- **목표**: 실제 MMORPG/액션 서버와 유사한 인게임 동선 및 부하 제어
- **서버 구현**:
  - 전체 맵에 있는 모든 유저에게 브로드캐스팅할 때 발생하는 $O(N^2)$ 패킷 폭발 문제를 해결하기 위해 맵을 격자(**Sector / Grid**)로 분할
  - 내 주변 9개 섹터(시야 범위, Area of Interest)에 있는 플레이어에게만 이동/공격 패킷을 전송하도록 최적화
- **CLI 더미 클라이언트 구현**:
  - **더미 봇 상태 머신(FSM)**: `CONNECTING` -> `LOGIN` -> `ENTER_ROOM` -> `PATROL_AND_ATTACK` -> `DISCONNECT`
  - CLI 명령어 인터페이스를 제공하여 실시간으로 부하를 조절:
    ```text
    [DummyCLI] > spawn 1000        # 1000개 봇 점진적 접속 (Ramp-up)
    [DummyCLI] > move_rate 10      # 초당 10회 이동 패킷 전송
    [DummyCLI] > stat              # 현재 연결 수, Send/Recv TPS, 평균/P99 RTT 출력
    [DummyCLI] > chaos on          # 무작위 재접속/절단 테스트 활성화
    ```
