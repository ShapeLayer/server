# Part B-3. 게임 서버 동시성 아키텍처 패턴

IOCP나 `epoll`을 통해 패킷을 초당 수십만 개씩 받아내더라도, 그 패킷이 건드리는 인게임 상태(`Room`, `Player`, `Monster`, `Inventory`)를 멀티스레드에서 어떻게 동기화하느냐에 따라 서버의 운명이 갈린다.

전통적인 **Lock 기반 공유 메모리 모델**이 왜 복잡한 게임 서버에서 실패하는지 살펴보고, 현대 게임 서버의 표준 아키텍처인 **Job Queue(Actor 모델 / Command 패턴)**와 이를 뒷받침하는 **Lock-Free 자료구조**의 원리를 심층 분석한다.

---

## 1. Lock 기반 공유 메모리 방식의 한계

초창기 멀티스레드 서버는 공유 객체마다 `Mutex`를 붙이는 방식을 사용했다. 이때 락의 범위(Granularity)에 따라 두 가지 극단적인 딜레마에 빠진다.

### 1.1 Coarse-grained Lock (거친/큰 단위의 락)
- 예: `GameRoom` 전체나 `World` 전체에 거대한 `Mutex` 하나만 두는 방식.
- **문제점 (심각한 병목과 직렬화)**:
  - 한 방(Room)에 200명의 유저가 있을 때, 1번 유저가 물약을 마시는 동안 나머지 199명의 이동, 공격, 채팅 패킷을 처리하려는 워커 스레드들이 모두 `roomMutex.lock()` 앞에서 줄을 서서 블로킹된다.
  - 16코어 CPU를 달아놓아도 실제로는 한 번에 1개의 스레드만 일하고 15개는 잠만 자는(Lock Contention + Context Switching 폭증) 최악의 효율을 보인다.

### 1.2 Fine-grained Lock (세밀한/작은 단위의 락)
- 병목을 줄이기 위해 `Player`, `Monster`, `Inventory`, `Party`, `Tile`마다 각각의 개별 `Mutex`를 잘게 쪼개어 넣는 방식.
- **문제점 (데드락 지옥과 콘텐츠 개발 생산성 붕괴)**:
  - 게임 기획은 끊임없이 변한다. "광역 스킬로 몬스터 5마리를 때렸는데, 그중 1마리가 반사 버프가 있어서 내 체력이 깎이고, 내가 죽으면서 파티원들에게 사망 알림을 보낸다"라는 로직을 짜려면 도대체 몇 개의 락을 어떤 순서로 잡아야 할까?
  - 신규 콘텐츠 프로그래머가 실수로 락 순서를 하나라도 어기면 라이브 서버가 데드락으로 멈추고, 락을 하나라도 빼먹으면 레이스 컨디션으로 아이템 복사 버그가 터진다.
  - 또한 함수 하나를 실행하기 위해 작은 `Mutex`를 수십 번 잠갔다 풀었다 반복하는 오버헤드 자체가 배보다 배꼽이 더 커진다.

---

## 2. Job Queue 패턴 (Actor 모델 / Command 패턴)

위 딜레마를 해결하기 위해 현대 게임 서버가 채택한 패러다임 전환은 다음과 같다:
> **"여러 스레드가 공유 메모리에 직접 달려들어 Lock을 걸고 데이터를 고치게 하지 말자! 대신 할 일(함수 + 인자)을 캡슐화한 일감(`Job`)으로 만들어 해당 객체(`Room`)의 우편함(`JobQueue`)에 던져 넣고, 한 번에 딱 하나의 스레드만 그 우편함을 열어서 순서대로 처리하게 만들자!"**

이것이 디자인 패턴의 **Command 패턴**과 분산/동시성 이론의 **Actor 모델(Erlang, Akka 등)**을 C++ 게임 서버에 맞게 결합한 **Job Queue (작업 큐 / Serializer)** 아키텍처다.

---

### 2.1 Job Queue 아키텍처의 동작 흐름

```text
[Network Worker Threads]                  [GameRoom (Actor / JobQueue)]
           |                                            |
 Thread 1: Move 패킷 파싱 완료                          |
  --> room->DoAsync(&Room::HandleMove, p1, pos) ------> | [ Job 1: HandleMove ]
  (기다리지 않고 즉시 리턴하여 다음 패킷 수신하러 감!)   | [ Job 2: HandleSkill ]
                                                        | [ Job 3: MonsterAI ]
 Thread 2: Skill 패킷 파싱 완료                         |          |
  --> room->DoAsync(&Room::HandleSkill, p2, id) ------> |          v
                                                        |  (단 1개의 스레드만 진입하여
                                                        |   Lock 없이 순차적으로 실행!)
```

1. **일감 캡슐화 (`Job`)**: 실행할 멤버 함수 포인터(또는 `std::function<void()>`, 람다)와 매개변수들을 묶어 `Job` 객체로 만든다.
2. **비동기 투입 (`Push`)**: 패킷을 수신한 I/O 스레드는 `Room`의 데이터를 직접 건드리지 않고, `room->PushJob(job)`으로 큐에 일감을 밀어 넣기만 한 뒤 **블로킹 없이 즉시 반환**된다.
3. **단일 스레드 실행 보장 (Single-threaded Execution Guarantee)**:
   - 특정 순간에 하나의 `JobQueue`(`GameRoom`) 내부의 일감들을 꺼내어 실행(`Execute`)할 수 있는 워커 스레드는 **오직 단 1개뿐**이다!
   - **핵심 효과**: `GameRoom::HandleMove()`, `GameRoom::HandleSkill()` 같은 인게임 콘텐츠 함수 내부에서는 **동시에 두 스레드가 들어오는 일이 절대 없으므로, `Mutex` 락을 단 한 줄도 칠 필요가 없다!** 데드락과 레이스 컨디션으로부터 콘텐츠 프로그래머가 완벽히 해방된다.

---

### 2.2 C++ Job Queue 핵심 구현 메커니즘

어떻게 `Mutex`로 방 전체를 잠그지 않으면서 "한 번에 하나의 스레드만 큐를 비운다"는 규칙을 보장할 수 있을까? 핵심은 **원자적 일감 카운터(`std::atomic<int32_t> _jobCount`)**에 있다.

```cpp
class JobQueue : public std::enable_shared_from_this<JobQueue>
{
public:
    void Push(JobRef job)
    {
        // 1. 큐에 일감을 넣기 전에 먼저 원자적으로 카운트를 1 증가시킨다.
        // fetch_add는 증가시키기 직전의 이전 값을 반환한다.
        const int32_t prevCount = _jobCount.fetch_add(1, std::memory_order_acq_rel);

        // 2. LockQueue(또는 MPSC Lock-Free Queue)에 실제 일감 데이터 삽입
        _jobs.Push(job);

        // 3. 만약 내가 넣기 전에 카운트가 0이었다면?
        // 현재 이 JobQueue를 담당해서 실행 중인 스레드가 아무도 없다는 뜻이다!
        if (prevCount == 0)
        {
            // 내가 총대를 메고 직접 실행하거나, GlobalQueue에 이 JobQueue를 등록한다!
            Execute(); 
        }
        // 만약 prevCount > 0 이었다면?
        // 이미 다른 스레드가 이 JobQueue의 Execute() 루프를 돌고 있으므로,
        // 나는 큐에 일감만 넣어두고 즉시 퇴근하면 그 스레드가 내 일감까지 알아서 처리해 준다!
    }

    void Execute()
    {
        while (true)
        {
            std::vector<JobRef> jobs;
            _jobs.PopAll(OUT jobs); // 현재 쌓여있는 일감들을 한 번에 쓸어 담음

            const int32_t jobCount = static_cast<int32_t>(jobs.size());
            for (int32_t i = 0; i < jobCount; i++)
            {
                jobs[i]->Execute(); // [핵심] 여기서는 락 없이 안전하게 단일 스레드로 실행!
            }

            // 처리한 개수만큼 카운트를 차감했는데 정확히 0이 되었다면?
            // 더 이상 밀린 일감이 없으므로 깔끔하게 실행 종료!
            if (_jobCount.fetch_sub(jobCount, std::memory_order_acq_rel) == jobCount)
                break;

            // 만약 0이 아니라면? 내가 jobs를 실행하는 도중에 다른 스레드가 Push()로
            // 새 일감을 몰래 더 넣어두고 갔다는 뜻이므로, while 루프를 한 바퀴 더 돈다!
        }
    }

private:
    LockQueue<JobRef> _jobs;
    std::atomic<int32_t> _jobCount{0};
};
```

---

### 2.3 GlobalQueue(전역 큐)와 JobTimer(예약 실행기)

위의 기본 `Push -> Execute` 구조만 쓰면 두 가지 실무적 문제가 발생한다:
1. **일감 독식 (Starvation)**: 만약 1번 방에 유저 500명이 몰려 쉴 새 없이 `Push`가 들어오면, 우연히 `prevCount == 0`에 걸려 1번 방의 `Execute()`에 들어간 스레드는 `while` 루프를 영원히 빠져나오지 못하고 1번 방에 감금된다.
2. **예약 실행 불가**: "3초 뒤에 독 데미지 적용", "0.2초마다 몬스터 AI 실행"처럼 미래의 특정 시점에 실행되어야 할 일감을 처리할 수 없다.

이를 해결하기 위해 **`GlobalQueue`**와 **`JobTimer`**를 결합한다.

```text
                       +-----------------------------------+
                       |  JobTimer (Priority Queue: 최소힙) |
                       |  [100ms 뒤 AI] [300ms 뒤 스킬쿨]  |
                       +-----------------------------------+
                                         | (시간이 다 된 Job을 해당 JobQueue에 Push)
                                         v
[I/O 패킷 수신] -----> [Room A JobQueue] / [Room B JobQueue]
                                         |
                                         v (실행할 차례가 된 JobQueue 포인터를 등록)
                       +-----------------------------------+
                       |     GlobalQueue (전역 배분 큐)     |
                       |   [ Room A ] -> [ Room C ] ...    |
                       +-----------------------------------+
                         /               |               \
                        v                v                v
                 [Worker Thread 1] [Worker Thread 2] [Worker Thread 3]
                 (각자 타임 슬라이스만큼만 Room의 일감을 처리하고 반납!)
```

- **GlobalQueue와 타임 슬라이스(Time-Slice)**:
  - 스레드가 한 방(`JobQueue`)에 감금되는 것을 막기 위해, 각 스레드(TLS)에 **최대 실행 허용 시간(예: 64ms)**을 부여한다.
  - 스레드가 `Room A`의 일감을 처리하다가 64ms가 지나면, 아직 일감이 남아있더라도 루프를 중단하고 `Room A`의 포인터를 **`GlobalQueue`** 맨 뒤에 다시 줄 세운 뒤 다른 방(`Room B`)을 처리하러 간다. 이로써 모든 방이 공평하게 CPU를 할당받는다.
- **JobTimer (우선순위 큐 기반 스케줄러)**:
  - 실행될 목표 시각(`executionTick`)을 기준으로 정렬되는 **최소 힙(`std::priority_queue`)**이다.
  - 워커 스레드들이 주기적으로 `JobTimer::Distribute(currentTick)`를 호출하여, 만료 시각이 지난 일감들을 꺼내 해당 `JobQueue`로 `Push`해 준다. 이를 통해 별도의 OS 타이머 스레드 없이도 수만 마리 몬스터의 AI 틱과 버프 지속 시간을 정밀하게 처리할 수 있다.

---

## 3. Lock-Free 자료구조 기초

Job Queue에 일감을 넣을 때(`_jobs.Push`)나 메모리 풀에서 객체를 꺼낼 때조차 아주 짧은 순간의 `Mutex` 경합을 피하고 싶다면 **Lock-Free(비잠금) 자료구조**를 사용한다.

> **Wait-Free vs Lock-Free**:
> - **Lock-Free**: 여러 스레드가 동시에 접근할 때, 설령 일부 스레드가 CAS에 실패해 재시도 루프를 돌더라도 **전체 스레드 중 최소한 1개의 스레드는 반드시 유한한 단계 내에 작업을 성공하고 전진(System-wide Progress)**함이 보장되는 알고리즘이다. (특정 스레드가 영원히 잠들어서 전체 시스템을 멈추게 하는 일은 절대 없다.)

---

### 3.1 CAS (Compare-And-Swap) 원자적 연산의 원리

모든 Lock-Free 알고리즘의 심장은 CPU 하드웨어가 단일 명령어(`LOCK CMPXCHG`)로 원자성을 보장해 주는 **CAS(Compare-And-Swap)** 연산이다.

#### 동작 논리 (의사코드)
```cpp
// 하드웨어가 아래 과정을 누구도 끼어들 수 없게(Atomically) 한 번에 처리함!
bool CompareAndSwap(T* targetAddress, T& expected, T desired)
{
    if (*targetAddress == expected)
    {
        *targetAddress = desired; // 예상한 값과 일치하면 새 값으로 교체 성공!
        return true;
    }
    else
    {
        expected = *targetAddress; // 누군가 그사이에 값을 바꿨다면! 현재 값을 expected에 담아주고 실패 반환
        return false;
    }
}
```

#### C++ 표준 API: `compare_exchange_weak` vs `compare_exchange_strong`
- **`compare_exchange_weak`**: ARM 같은 LL/SC(Load-Linked / Store-Conditional) 아키텍처에서, 실제로는 값이 바뀌지 않았는데도 하드웨어 인터럽트 등 때문에 간혹 실패(`false`)를 반환하는 **Spurious Failure(가짜 실패)**가 발생할 수 있다. 대신 명령어 비용이 더 저렴하므로, 어차피 **`while` 루프 안에서 반복 시도하는 경우에는 항상 `weak`를 쓰는 것이 정석**이다.
- **`compare_exchange_strong`**: 가짜 실패 없이 값이 다를 때만 정확히 실패한다. 루프 없이 단발성으로 CAS를 1회만 시도할 때 사용한다.

#### Lock-Free Stack의 `Push` 구현 (CAS 루프의 전형)
```cpp
struct Node
{
    T data;
    Node* next;
};

class LockFreeStack
{
public:
    void Push(const T& value)
    {
        Node* newNode = new Node{value, nullptr};

        // 1. 현재 스택의 맨 위(head)를 읽어온다.
        newNode->next = _head.load(std::memory_order_relaxed);

        // 2. CAS 루프: 내가 읽었던 _head가 여전히 그대로라면 newNode로 바꿔치기!
        // 만약 그사이에 다른 스레드가 끼어들어 _head가 바뀌었다면,
        // compare_exchange_weak가 자동으로 newNode->next에 최신 _head 값을 갱신해 주므로 바로 재시도됨!
        while (!_head.compare_exchange_weak(
            newNode->next, 
            newNode, 
            std::memory_order_release, 
            std::memory_order_relaxed))
        {
            // 빈 루프
        }
    }

private:
    std::atomic<Node*> _head{nullptr};
};
```

---

### 3.2 Lock-Free의 치명적 함정: ABA 문제와 해결책

Lock-Free Stack에서 `Push`는 위처럼 간단하지만, 데이터를 꺼내고 메모리를 해제하는 **`Pop` 연산**을 똑같이 구현하면 서버가 알 수 없는 메모리 오염으로 크래시난다. 그 주범이 바로 **ABA 문제**와 **메모리 조기 해제(Use-After-Free)**다.

#### ① ABA 문제 발생 시나리오

```text
초기 스택 상태: Top -> [ Node A ] -> [ Node B ] -> [ Node C ]
```

1. **Thread 1**이 `Pop()`을 시작한다. 현재 `Top`인 `Node A`와 그 다음 노드인 `Node B`(`next = B`)를 읽었다.
   - 이제 Thread 1은 `CAS(&Top, expected = A, desired = B)`를 실행하기 직전에 **OS 스케줄러에 의해 타임 슬라이스를 뺏겨 잠시 멈춘다(Preempted)!**
2. 그사이에 **Thread 2**가 나타나서 번개같이 작업을 수행한다:
   - `Pop()` 호출: `Node A`를 꺼내고 `delete A`로 메모리를 해제(또는 풀에 반납)함. (스택: `Top -> B -> C`)
   - `Pop()` 호출: `Node B`도 꺼내고 `delete B`로 해제함! (스택: `Top -> C`)
   - `Push()` 호출: 새 노드를 할당(`new Node`)했는데, 메모리 할당자(또는 Object Pool)가 **방금 해제된 `Node A`의 메모리 주소를 그대로 재사용**하여 돌려줌!
   - 이 새 노드를 스택에 넣음. 결과적으로 스택은 **`Top -> [ Node A (주소는 같지만 완전히 다른 새 노드) ] -> [ Node C ]`** 상태가 되었다!
3. 이제 잠들었던 **Thread 1**이 깨어나서 아까 멈췄던 `CAS(&Top, expected = A, desired = B)`를 실행한다.
4. **참사 발생**:
   - CAS는 오직 포인터 주소값만 비교한다. 현재 `Top`의 주소는 여전히 **`A`**다! (`A -> B -> A`로 돌아왔기 때문!)
   - 따라서 CAS는 **"아무 일도 없었구나!"라고 착각하고 `Top`을 이미 해제되어 쓰레기 값이 된 `Node B`로 덮어씌워 버린다!** 스택 전체가 파괴되고 서버는 즉시 폭발한다.

---

#### ② ABA 문제의 3가지 실무 해결책

##### 해결책 1: Double-Word CAS (16바이트 CAS) / Tagged Pointer (Stamp 기법)
포인터 주소만 비교하지 말고, 포인터 옆에 **버전 번호(Stamp / Pop 카운터)**를 함께 붙여서 `[포인터 주소 + 버전 카운터]`를 한 묶음으로 원자적 비교/교체하는 기법이다.
- Thread 2가 `A`를 빼고 다시 넣는 과정에서 버전 카운터가 `1 -> 4`로 증가하므로, Thread 1이 깨어나서 `(주소: A, 버전: 1)`로 CAS를 시도하면 현재 값은 `(주소: A, 버전: 4)`이므로 **CAS가 안전하게 실패**하여 ABA 문제를 완벽히 막아낸다.
- **구현 방법**:
  1. **128-bit CAS (`CMPXCHG16B`, Windows `InterlockedCompareExchange128`)**: 64비트 포인터 + 64비트 카운터 구조체(16바이트 정렬 `alignas(16)`)를 한 번에 CAS 처리한다.
  2. **64-bit Tagged Pointer (비트 패킹)**: 현대 64비트 OS(x64)는 가상 메모리 주소로 64비트를 다 쓰지 않고 **하위 48비트**만 사용한다. 따라서 **남는 상위 16비트에 버전 스탬프(0~65535)를 우겨 넣어** 일반 64비트 `std::atomic<uint64_t>` 하나로 포인터와 버전을 동시에 CAS 처리한다!

```cpp
// 64비트 Tagged Pointer 구조 (상위 16비트: Stamp, 하위 48비트: Node 포인터)
struct TaggedPtr
{
    static constexpr uint64_t PTR_MASK = (1ULL << 48) - 1;

    static uint64_t Pack(Node* ptr, uint16_t stamp)
    {
        return (static_cast<uint64_t>(stamp) << 48) | (reinterpret_cast<uint64_t>(ptr) & PTR_MASK);
    }
    static Node* ExtractPtr(uint64_t packed)
    {
        return reinterpret_cast<Node*>(packed & PTR_MASK);
    }
    static uint16_t ExtractStamp(uint64_t packed)
    {
        return static_cast<uint16_t>(packed >> 48);
    }
};
```

##### 해결책 2: Hazard Pointer (위험 포인터)
Thread 1이 `Node A`를 읽고 `A->next`에 접근하려는 순간에 Thread 2가 `delete A`를 해버리면(메모리가 OS에 반납되면) Tagged Pointer를 쓰더라도 `A->next`를 읽는 순간 Access Violation이 날 수 있다.
- **원리**: 각 스레드가 전역적으로 볼 수 있는 **Hazard Pointer 목록**에 *"나 지금 `Node A` 주소를 읽고 있으니 아무도 이거 진짜 `delete` 하지 마!"*라고 깃발을 꽂아둔다.
- 다른 스레드가 `Pop`으로 노드를 빼더라도 즉시 `delete` 하지 않고 은퇴 목록(Retired List)에 모아두었다가, 현재 그 어떤 스레드의 Hazard Pointer에도 등록되어 있지 않을 때만 실제로 메모리를 해제한다.

##### 해결책 3: Epoch-Based Reclamation (EBR) 또는 Lock-Free Free-List (메모리 무해제 풀링)
게임 서버에서 가장 실용적으로 쓰는 방식이다.
- Lock-Free 자료구조의 노드들을 절대 `delete`(OS에 반납)하지 않고, **노드 전용 Object Pool 안에서만 계속 순환**시킨다.
- 이렇게 하면 어떤 스레드가 `A->next`를 읽는 도중 다른 스레드가 `A`를 풀에 반납하더라도 `A`의 메모리 페이지 자체는 항상 유효하므로 Access Violation이 나지 않으며, 여기에 **Tagged Pointer(64비트 비트 패킹)**만 결합하면 ABA 문제까지 깔끔하게 해결된다.

---

### 3.3 Lock-Free Queue의 분류 (SPSC, MPSC, MPMC)

큐(Queue)는 양 끝단(`head`와 `tail`)을 모두 건드려야 하므로 스택보다 구현이 까다롭다. 실무에서는 누가 넣고 누가 빼느냐에 따라 가장 가벼운 큐를 선택한다.

| 종류 | 의미 | 특징 및 게임 서버 활용처 |
| :--- | :--- | :--- |
| **SPSC** | Single-Producer Single-Consumer | 생산자 1명, 소비자 1명. **CAS조차 필요 없으며**, 링버퍼에 `acquire`/`release` 메모리 배리어만 걸면 가장 빠른 속도로 동작함. (예: 메인 스레드 $\leftrightarrow$ 단일 전용 DB/로그 스레드 통신) |
| **MPSC** | Multi-Producer Single-Consumer | **게임 서버의 `JobQueue`에 가장 적합한 모델!** 여러 네트워크 워커 스레드가 동시에 `Job`을 밀어 넣지만(Multi-Producer), 실행(`Execute`)은 한 번에 단 1개의 스레드만 꺼내어 수행(Single-Consumer)하기 때문. `Push`에만 원자적 `exchange`를 쓰고 `Pop`은 락 없이 초고속으로 처리 가능(예: Vyukov MPSC Queue). |
| **MPMC** | Multi-Producer Multi-Consumer | 여러 스레드가 동시에 넣고 동시에 빼는 범용 큐(예: Michael-Scott Queue, 혹은 링버퍼 슬롯마다 시퀀스 번호를 두는 Bounded MPMC Queue). **`GlobalQueue`나 스레드 풀의 공용 작업 큐**에 사용됨. |

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **Lock 모델 vs Job Queue (Actor 모델)**: 객체마다 락을 거는 방식은 경합(Contention)과 데드락(AB-BA) 위험으로 인해 복잡한 인게임 로직에 부적합하다. 대신 `Room` 단위로 **Job Queue**를 두어 워커 스레드가 일감(`Job`)만 등록하고, **한 번에 단 하나의 스레드만 큐를 비우며 순차 실행**하도록 만들면 콘텐츠 코드에서 Lock을 완전히 제거할 수 있다.
2. **GlobalQueue와 JobTimer**: 특정 방의 일감이 폭주해 스레드를 독점하는 것을 막기 위해 TLS 타임 슬라이스와 **GlobalQueue**를 두고, 쿨타임이나 AI 틱 같은 지연 실행은 우선순위 큐 기반의 **JobTimer**로 분배한다.
3. **CAS와 ABA 문제**: Lock-Free 자료구조에서 포인터 주소가 `A -> B -> A`로 재사용될 때 CAS가 변화를 감지하지 못해 자료구조가 깨지는 현상이 **ABA 문제**다. 이를 해결하기 위해 **Tagged Pointer(상위 16비트 버전 스탬프 패킹 / Double-word CAS)**나 **Hazard Pointer / Object Pool**을 함께 사용한다.
