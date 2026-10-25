# Part B-1. CPU 아키텍처와 메모리 계층

멀티스레드 게임 서버를 설계할 때 단순히 C++ 문법(`std::thread`, `std::mutex`, `std::atomic`)만 아는 것으로는 부족하다. 현대 멀티코어 CPU가 메모리에 어떻게 접근하는지, 스레드가 교체될 때 하드웨어 내부에서 무슨 일이 벌어지는지 이해해야만 눈에 보이지 않는 심각한 성능 저하와 재현하기 힘든 동시성 버그를 막을 수 있다.

본 문서에서는 Context Switching의 실제 하드웨어 비용, CPU 캐시 일관성(MESI)과 False Sharing, 그리고 명령어 재배치(Reordering)와 C++ 메모리 모델(`std::memory_order`)을 다룬다.

---

## 1. Context Switching(문맥 교환)의 하드웨어적 비용

OS 스케줄러가 CPU 코어에서 실행 중인 스레드 A를 내리고 스레드 B를 올리는 과정을 **Context Switching**이라고 한다. 흔히 "레지스터 값을 백업하고 복원하는 비용" 정도로만 알고 있지만, 실제로는 **직접 비용(Direct Cost)**보다 **간접 비용(Indirect Cost - 캐시 오염)**이 수십 배 더 치명적이다.

### 1.1 직접 비용 (Direct Cost)
1. **User Mode $\rightarrow$ Kernel Mode 전환**: 타이머 인터럽트나 시스템 콜(`Sleep`, `Mutex` 대기 등)로 인해 커널 모드로 진입한다.
2. **CPU 레지스터 저장 및 복원**: 현재 스레드 A의 범용 레지스터(`RAX`, `RBX` 등), 스택 포인터(`RSP`), 명령어 포인터(`RIP`), 부동소수점/SIMD 레지스터(`XMM`, `YMM`)를 커널 내부의 **TCB(Thread Control Block)** 메모리에 저장하고, 새로 실행할 스레드 B의 TCB에서 레지스터 값들을 CPU로 불러온다.
3. **스케줄러 알고리즘 실행**: 커널이 다음에 실행할 스레드를 선택하는 연산 비용.

### 1.2 간접 비용 (Indirect Cost: 진짜 성능 저하의 주범)
스레드 B로 문맥 교환이 끝난 직후, 스레드 B가 코드를 실행하기 시작할 때 하드웨어 내부에서는 **Cold Cache(차가워진 캐시)** 현상이 발생한다.

```text
[Context Switch 직후 CPU 코어 내부 상황]
+-------------------------------------------------------------------+
|  L1 / L2 Cache: 스레드 A가 쓰던 데이터로 가득 참 (스레드 B에겐 쓰레기) |
|  -> 스레드 B가 메모리에 접근할 때마다 연쇄적인 Cache Miss 발생!       |
|  -> 느린 메인 메모리(RAM)까지 다녀오느라 수백 CPU 사이클 정지(Stall)  |
+-------------------------------------------------------------------+
```

1. **CPU L1/L2/L3 캐시 오염 (Cache Pollution)**:
   - L1 캐시 접근은 약 **1ns(약 3~4 사이클)**가 걸리지만, 메인 메모리(RAM) 접근은 약 **60~100ns(수백 사이클)**가 걸린다.
   - 스레드 A가 코어에서 돌면서 L1/L2 캐시에 자신의 지역 변수와 객체 데이터를 잔뜩 올려두었는데, 갑자기 스레드 B로 바뀌면 스레드 B가 접근하는 데이터는 캐시에 없으므로 **대규모 Cache Miss**가 쏟아진다.
2. **TLB (Translation Lookaside Buffer) 플러시**:
   - 가상 메모리 주소를 물리 메모리 주소로 변환해 주는 하드웨어 캐시인 **TLB**는 프로세스 전환(Process Context Switch, 페이지 테이블 레지스터 `CR3` 변경) 시 무효화된다.
   - 같은 프로세스 내의 **스레드 간 전환(Thread Context Switch)**에서는 주소 공간을 공유하므로 TLB가 플러시되지 않는다는 큰 장점이 있다(이것이 멀티프로세스 대신 멀티스레드 서버를 쓰는 핵심 이유 중 하나다).
3. **분기 예측기 (Branch Predictor) 및 파이프라인 초기화**:
   - CPU 파이프라인이 비워지고 분기 예측 히스토리도 어긋나면서 초기 명령어 처리 효율이 급감한다.

> **결론**: 고성능 게임 서버는 **"워커 스레드 개수를 물리/논리 코어 개수에 맞추고, 스레드가 블로킹되어 Context Switching이 일어나는 일 자체를 최소화(Non-blocking, Spin/Async, Job Queue)"**하도록 설계해야 한다.

---

## 2. 캐시 일관성(Cache Coherency)과 False Sharing

### 2.1 메모리 계층 구조와 캐시 라인 (Cache Line)

CPU는 메인 메모리(RAM)에서 데이터를 읽어올 때 `int` 하나(4바이트)만 달랑 가져오지 않는다. 공간 지역성(Spatial Locality)을 활용하기 위해 항상 **캐시 라인(Cache Line)**이라 불리는 **64바이트(Byte) 블록 단위**로 묶어서 L1/L2/L3 캐시로 퍼 올린다.

```text
                 [Core 0]                 [Core 1]
             +--------------+         +--------------+
             | Registers    |         | Registers    |
             +--------------+         +--------------+
             | L1 Cache 32K |         | L1 Cache 32K |  (~1 ns)
             +--------------+         +--------------+
             | L2 Cache 512K|         | L2 Cache 512K|  (~4 ns)
             +--------------+         +--------------+
                    \                         /
             +---------------------------------------+
             |          Shared L3 Cache (LLC)        |  (~15 ns)
             +---------------------------------------+
                                 |
             +---------------------------------------+
             |            Main Memory (RAM)          |  (~80 ns)
             +---------------------------------------+
  * 모든 캐시와 RAM 간의 데이터 전송 기본 단위는 64 Byte (1 Cache Line)
```

---

### 2.2 캐시 일관성 프로토콜 (MESI 프로토콜)

멀티코어 환경에서 Core 0과 Core 1이 동일한 메모리 주소(또는 동일한 64바이트 캐시 라인)를 각자의 L1 캐시에 올려두고 읽고 쓸 때, 두 코어의 캐시 데이터가 서로 달라지는 문제를 막기 위해 하드웨어는 **MESI 프로토콜**을 사용한다.

각 캐시 라인은 다음 4가지 상태 중 하나를 가진다:

| 상태 (State) | 의미 | 동작 특징 |
| :--- | :--- | :--- |
| **M (Modified)** | **수정됨** | 현재 코어의 캐시에만 존재하며, RAM의 원본보다 최신 값으로 수정된 상태(Dirty). 다른 코어가 이 라인을 읽으려 하면 먼저 RAM에 쓰거나(Write-back) 전달해야 함. |
| **E (Exclusive)** | **독점** | 현재 코어의 캐시에만 존재하며, RAM의 원본과 값이 일치하는 상태(Clean). 다른 코어가 갖고 있지 않으므로 버스 통신 없이 즉시 **M** 상태로 바꾸어 수정 가능. |
| **S (Shared)** | **공유됨** | 2개 이상의 코어가 이 캐시 라인을 동시에 읽어들여 가지고 있으며, RAM과 일치하는 상태. 읽기만 할 때는 아무 문제가 없으나, **누군가 값을 수정하려 하면 다른 모든 코어의 캐시 라인을 무효화(Invalidate)시켜야 함.** |
| **I (Invalid)** | **무효화됨** | 다른 코어가 데이터를 수정하는 바람에 내 캐시 라인의 데이터가 상해버린(Stale) 상태. 다시 읽으려면 캐시 미스(Cache Miss)가 발생하여 새로 가져와야 함. |

---

### 2.3 False Sharing (거짓 공유) 현상과 해결책

**False Sharing(거짓 공유)**이란, 두 스레드가 코드상으로는 **전혀 다른 독립적인 변수**를 수정하고 있음에도 불구하고, 그 두 변수가 우연히 **메모리상에서 인접하여 동일한 64바이트 캐시 라인(Cache Line)에 함께 묶여 있을 때** 발생하는 심각한 하드웨어 성능 저하 현상이다.

#### 문제 발생 시나리오 (Cache Line Ping-Pong)
```cpp
struct ThreadStats
{
    int64_t thread0PacketCount; // 8 Byte (Offset 0 ~ 7)
    int64_t thread1PacketCount; // 8 Byte (Offset 8 ~ 15)
}; // 두 변수가 나란히 붙어 있어 동일한 64 Byte 캐시 라인 안에 들어감!
```

1. **Thread 0 (Core 0)**는 오직 `thread0PacketCount++`만 수행하고, **Thread 1 (Core 1)**은 오직 `thread1PacketCount++`만 수행한다.
2. 논리적으로는 두 스레드가 서로 다른 변수를 건드리므로 락(Lock)도 필요 없고 아무런 경합이 없어야 정상이다.
3. **하드웨어의 실제 동작**:
   - Core 0이 `thread0PacketCount`를 1 증가시키는 순간, MESI 프로토콜에 의해 해당 64바이트 캐시 라인 전체가 Core 0에서는 **M(Modified)** 상태가 되고, Core 1에 있던 캐시 라인 전체는 강제로 **I(Invalid, 무효화)** 처리된다!
   - 직후 Core 1이 자신의 변수인 `thread1PacketCount`를 수정하려고 보면, 캐시 라인이 **I(Invalid)** 상태이므로 **강제 Cache Miss**가 발생하여 Core 0의 캐시 내용이 플러시될 때까지 기다렸다가 다시 읽어와야 한다. 그리고 Core 1이 값을 수정하면 이번엔 Core 0의 캐시 라인이 **I(Invalid)**가 된다.
   - 이를 **캐시 라인 핑퐁(Cache Line Ping-Pong)**이라 부르며, 멀티스레드로 돌렸는데도 싱글스레드보다 **5배~10배 이상 느려지는 기현상**을 초래한다.

#### 해결책: `alignas(64)` (캐시 라인 패딩 / 정렬)
서로 다른 스레드가 빈번하게 수정(Write)하는 변수나 구조체는 반드시 **`alignas(64)`** (또는 C++17의 `std::hardware_destructive_interference_size`)를 지정하여 **서로 다른 캐시 라인에 배치되도록 강제 분리**해야 한다.

```cpp
struct ThreadStats
{
    // 각 변수를 64바이트 경계에 정렬하여 서로 다른 캐시 라인에 독점 배치함
    alignas(64) int64_t thread0PacketCount; 
    alignas(64) int64_t thread1PacketCount; 
};
```
```text
[alignas(64) 적용 후 메모리 레이아웃]
|<------------ Cache Line 0 (64 Byte) ------------>|<------------ Cache Line 1 (64 Byte) ------------>|
[ thread0PacketCount (8B) |   Padding (56 Byte)    ][ thread1PacketCount (8B) |   Padding (56 Byte)    ]
  -> Core 0이 단독으로 독점(E/M 상태 유지)!           -> Core 1이 단독으로 독점(E/M 상태 유지)!
```
> **실무 팁**: Lock-Free Queue를 구현할 때 생산자(Producer)가 수정하는 `tail` 인덱스와 소비자(Consumer)가 수정하는 `head` 인덱스를 구조체에 나란히 선언하면 False Sharing이 직격탄으로 발생한다. 반드시 `alignas(64)`로 `head`와 `tail`을 분리해야 한다.

---

## 3. Memory Visibility(가시성) & Reordering(명령어 재배치)

멀티스레드 환경에서 `Mutex` 없이 공유 변수를 다룰 때 우리를 가장 괴롭히는 두 가지 현상은 **가시성(Visibility)**과 **명령어 재배치(Instruction Reordering)**다.

### 3.1 왜 내가 쓴 코드 순서대로 실행되지 않는가?

```cpp
// 전역 공유 변수 (초기값 모두 0 / false)
int payload = 0;
bool ready = false;

// [Thread A: 생산자]
void Producer()
{
    payload = 42;  // (1) 데이터 기록
    ready = true;  // (2) 준비 완료 플래그 켜기
}

// [Thread B: 소비자]
void Consumer()
{
    while (!ready) { } // (3) 플래그가 켜질 때까지 대기
    assert(payload == 42); // (4) 과연 항상 성공할까? -> 실패(Crash)할 수 있다!
}
```

상식적으로는 Thread A가 `(1) payload = 42`를 먼저 실행한 뒤 `(2) ready = true`를 실행했으므로, Thread B가 `(3) ready == true`를 확인하고 루프를 빠져나왔다면 `(4) payload`는 무조건 `42`여야 한다. 그러나 실제 하드웨어에서는 `payload`가 `0`으로 읽히는 기괴한 일이 발생한다.

#### 원인 1: 컴파일러의 재배치 (Compiler Reordering)
컴파일러(최적화 옵션 `-O2`, `/O2`)는 단일 스레드 관점에서 `payload`와 `ready` 사이에 아무런 데이터 의존성이 없다고 판단하면, 레지스터 할당 효율이나 파이프라인 최적화를 위해 기계어 생성 순서를 **`(2) ready = true` $\rightarrow$ `(1) payload = 42`로 마음대로 뒤바꿀 수 있다.**

#### 원인 2: CPU 하드웨어의 비순차 실행 및 Store Buffer (Hardware Reordering)
컴파일러가 순서를 바꾸지 않았더라도, 현대 CPU는 **Store Buffer(쓰기 버퍼)**와 **Out-of-Order Execution(비순차 실행)** 때문에 메모리 반영 순서가 뒤집힌다.

```text
         [Core 0 (Producer)]                     [Core 1 (Consumer)]
                 |                                        |
      payload = 42; (Cache Miss!)                         |
      -> Store Buffer에 임시 보관                         |
                 |                                        |
      ready = true; (Cache Hit!)                          |
      -> L1 캐시에 즉시 반영됨! --------> ready == true 읽음!
                 |                        payload 읽음 (아직 0!) -> Assert 실패!
      (뒤늦게 Store Buffer의                              |
       payload = 42가 캐시에 반영됨)                      |
```

- 만약 `payload`는 현재 Core 0의 캐시에 없고(Cache Miss), `ready`는 이미 캐시에 올라와 있다면(Cache Hit):
- Core 0은 느린 `payload = 42` 쓰기 요청을 **Store Buffer**에 던져두고 기다리는 동안, 뒤이어 오는 `ready = true`를 먼저 캐시에 반영해 버린다(**Store-Store Reordering**)거나, 쓰기 버퍼가 비워지기 전에 다음 읽기 명령을 먼저 수행해 버린다(**Store-Load Reordering** - x86/x64에서도 발생!).

---

### 3.2 메모리 배리어 (Memory Barrier / Fence)와 C++ `std::memory_order`

이러한 컴파일러와 CPU의 재배치를 막고 코어 간 메모리 가시성을 강제하기 위해 하드웨어는 **메모리 배리어(Memory Barrier / Fence)** 명령어를 제공하며, C++11부터는 이를 추상화한 **`std::atomic`과 `std::memory_order`**를 표준으로 제공한다.

#### 4가지 핵심 `std::memory_order` 완벽 정리

##### ① `memory_order_relaxed` (완화된 순서)
- **보장하는 것**: 오직 **해당 변수 자체에 대한 읽기/쓰기의 원자성(Atomicity)**만 보장한다(값이 반쪽만 써지는 Torn Write 방지).
- **보장하지 않는 것**: 주변 일반 변수나 다른 atomic 연산과의 **순서(Reordering 방지)를 전혀 보장하지 않는다.**
- **주 사용처**: 다른 변수와의 실행 순서가 전혀 상관없는 **단순 통계 카운터**(예: 누적 패킷 수 증가 `totalPackets.fetch_add(1, std::memory_order_relaxed)`), 또는 객체의 참조 카운트 증가(`shared_ptr` 복사 시).

##### ② `memory_order_release` (쓰기 배리어 - 발행) & ③ `memory_order_acquire` (읽기 배리어 - 획득)
가장 실무에서 중요하게 쓰이는 **생산자-소비자 동기화 쌍(Pair)**이다.

```text
[Thread A: Producer]                              [Thread B: Consumer]
  일반 변수 읽기/쓰기 (예: payload = 42)                 |
  -------------------------------------           (이 위로 아래 코드가 넘어올 수 없음!)
  atomicVar.store(true, memory_order_release); ---> atomicVar.load(memory_order_acquire);
  (이 아래로 위 코드가 내려올 수 없음!)             -------------------------------------
                                                    일반 변수 읽기/쓰기 (예: print(payload))
```

- **`memory_order_release` (`store`에 사용)**:
  - **의미**: 이 `store` 연산 **이전(위쪽)에 작성된 모든 메모리 읽기/쓰기 명령들이, 이 `store` 연산 이후(아래쪽)로 넘어가는(Reordering) 것을 절대 금지**한다. 또한 이전까지 수정한 모든 내용을 다른 코어가 볼 수 있도록 발행(Publish)한다.
- **`memory_order_acquire` (`load`에 사용)**:
  - **의미**: 이 `load` 연산 **이후(아래쪽)에 작성된 모든 메모리 읽기/쓰기 명령들이, 이 `load` 연산 이전(위쪽)으로 끌어올려지는(Reordering) 것을 절대 금지**한다.
- **Acquire-Release 동기화(Synchronizes-with) 효과**:
  - 스레드 A가 같은 atomic 변수에 대해 `release`로 쓴 값을 스레드 B가 `acquire`로 읽는 순간, **스레드 A가 `release` 이전에 수행했던 모든 메모리 쓰기 작업(일반 변수 포함)이 스레드 B의 `acquire` 이후 코드에서 100% 완벽하게 보이는 것(Visibility)이 보장**된다!

```cpp
// Acquire-Release로 해결한 완벽한 코드
int payload = 0;
std::atomic<bool> ready{false};

void Producer()
{
    payload = 42; // (1) 일반 변수 쓰기 (절대 아래 줄 밑으로 재배치되지 않음!)
    ready.store(true, std::memory_order_release); // (2) Release Store
}

void Consumer()
{
    // (3) Acquire Load
    while (!ready.load(std::memory_order_acquire)) { } 
    // (4) 위 줄 위로 재배치되지 않으며, payload == 42임이 100% 보장됨!
    assert(payload == 42);
}
```
> **아키텍처 팁 (x86/x64 vs ARM)**: Intel/AMD의 **x86/x64 아키텍처는 기본적으로 강력한 메모리 모델(TSO, Total Store Order)**을 사용하여 하드웨어 차원에서 대부분의 순서(Load-Load, Load-Store, Store-Store)를 이미 보장한다. 따라서 x86/x64에서는 `acquire`와 `release`를 써도 추가적인 CPU 펜스 명령어(`mfence` 등)가 생성되지 않고 단지 컴파일러 재배치만 막아주므로 **성능 오버헤드가 사실상 0(공짜)**이다! 반면 모바일이나 최신 클라우드 서버(AWS Graviton, Apple Silicon)에 쓰이는 **ARM 아키텍처는 약한 메모리 모델(Weakly Ordered)**이라 `acquire`/`release`가 없으면 실제 하드웨어에서 순서가 마구 뒤섞인다.

##### ④ `memory_order_seq_cst` (Sequential Consistency, 순차적 일관성 - 기본값)
- `std::atomic` 연산 시 인자를 생략하면 기본으로 적용되는 가장 강력한 제약이다.
- **의미**: `acquire`/`release`의 모든 보장에 더해, **프로그램 내의 모든 스레드가 모든 `seq_cst` 연산들을 완전히 동일한 단일 전역 순서(Single Total Order)로 관측**하도록 보장한다(x86에서도唯一하게 허용되는 Store-Load 재배치까지 완벽히 차단함).
- **단점**: 이를 위해 x86에서도 무거운 하드웨어 명령어(`LOCK` 접두사 또는 `MFENCE`)를 강제 실행하여 Store Buffer를 전부 비워버리므로 `acquire`/`release`보다 수십 사이클 더 느리다.

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **Context Switching 비용**: 레지스터 백업 같은 직접 비용보다, 스레드 교체 후 **L1/L2/L3 CPU 캐시가 오염되어 발생하는 연쇄적인 Cache Miss(간접 비용)**가 훨씬 치명적이다.
2. **False Sharing (거짓 공유)**: 서로 다른 스레드가 독립적인 변수를 수정하더라도 두 변수가 **동일한 64바이트 캐시 라인**에 위치하면, MESI 프로토콜에 의해 상대 코어의 캐시 라인이 계속 무효화(**Invalid**)되는 캐시 핑퐁 현상이 발생한다. **`alignas(64)`**로 캐시 라인을 분리해 해결한다.
3. **`std::memory_order`**: 단순 카운터에는 `relaxed`, 생산자-소비자 간의 데이터 발행/구독 및 Lock 구현에는 오버헤드가 적고 가시성을 보장하는 **`release` / `acquire` 쌍**을 사용하며, 전역적인 단일 순서가 필요한 경우에만 기본값인 `seq_cst`를 사용한다.
