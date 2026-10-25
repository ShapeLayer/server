# Part B-2. 동기화 객체와 데드락(Deadlock)

멀티스레드 환경에서 공유 데이터(Shared State)의 무결성을 지키기 위해 운영체제와 표준 라이브러리는 다양한 동기화 객체(Synchronization Primitives)를 제공한다. 각 동기화 객체가 **User-mode(유저 모드)**와 **Kernel-mode(커널 모드)** 중 어디에서 어떻게 동작하는지 정확히 이해해야 상황에 맞는 최적의 락을 선택할 수 있으며, 멀티스레드 서버의 최대 적(Enemy)인 **데드락(Deadlock)**을 예방하고 런타임에 추적할 수 있다.

---

## 1. User-mode vs Kernel-mode 동기화 객체

동기화 객체는 경합(Contention)이 발생했을 때 스레드를 어떻게 대기시키느냐에 따라 크게 세 가지(순수 유저 모드, 순수 커널 모드, 하이브리드)로 나뉜다.

| 구분 | 동작 위치 | 대표 객체 | 특징 및 장단점 |
| :--- | :--- | :--- | :--- |
| **순수 User-mode** | 유저 공간 | `SpinLock` (`std::atomic_flag`) | 커널 전환(System Call)이나 Context Switch 없이 루프를 돌며 대기. 임계 영역이 극도로 짧을 때 가장 빠르지만, 길어지면 CPU 코어를 100% 낭비함. |
| **순수 Kernel-mode** | OS 커널 | Windows `HANDLE Mutex`, `Semaphore`, `Event` | 락 획득/해제 시 무조건 시스템 콜을 통해 커널 모드로 진입(수천 CPU 사이클 소모). 서로 다른 프로세스 간 동기화가 가능하나, 단일 서버 프로세스 내에서는 너무 무거워 거의 쓰지 않음. |
| **하이브리드 (User + Kernel)** | 평소엔 유저, 경합 시에만 커널 | Windows **`CRITICAL_SECTION`**, **`SRWLock`**, Linux **`futex`** (`pthread_mutex`, `std::mutex`) | **현대 게임 서버의 표준.** 경합이 없을 때는 유저 모드에서 원자적 연산(CAS) 단 한 번으로 끝내고, 이미 누가 락을 쥐고 있어 대기해야 할 때만(일정 스핀 후) 커널 모드로 진입해 스레드를 재움. |

---

### 1.1 SpinLock (스핀락)의 내부 원리와 최적화

SpinLock은 다른 스레드가 락을 소유하고 있을 때, 스레드를 블로킹(Context Switch)시키지 않고 **`while` 루프를 돌며(Spinning) 락이 풀릴 때까지 계속 확인**하는 동기화 기법이다.

#### ① 왜 잘못 짜인 SpinLock(`while(exchange)`)은 느린가? (TTAS 패턴)
초보자가 흔히 작성하는 SpinLock은 매 루프마다 `compare_exchange_weak`(CAS) 또는 `exchange`를 무조건 호출한다.
- 문제점: CAS나 `exchange`는 내부적으로 메모리에 **쓰기(Write)**를 시도하는 명령어다. 8개의 코어가 동시에 하나의 변수에 CAS를 연타하면, Part B-1에서 배운 **MESI 프로토콜의 캐시 무효화(Invalidate) 폭풍**이 일어나 메모리 버스가 마비된다.
- **해결책: TTAS (Test and Test-And-Set) 패턴 + `_mm_pause()`**
  1. 먼저 단순 읽기(`load`)로 락이 풀렸는지만 조용히 확인한다(모든 코어가 캐시 라인을 **S(Shared)** 상태로 공유하므로 버스 트래픽이 0이다!).
  2. 누군가 락을 풀어 `false`가 읽힌 순간에만 비로소 `exchange`(쓰기)를 시도한다.
  3. 루프 내부에 **`_mm_pause()` (`YieldProcessor()`)** 명령어를 넣어 CPU 파이프라인 과열과 전력 소모를 줄이고, 메모리 순서 위반(Memory Order Violation)으로 인한 파이프라인 플러시 페널티를 방지한다.

```cpp
#include <atomic>
#include <immintrin.h> // _mm_pause()

class SpinLock
{
public:
    void lock() noexcept
    {
        for (;;)
        {
            // 1차 시도: 락이 비어있다면 즉시 획득 (Acquire 메모리 배리어)
            if (!_locked.exchange(true, std::memory_order_acquire))
                return;

            // 2차 대기 (TTAS): 락이 풀릴 때까지 캐시 공유(Shared) 상태로 읽기(load)만 수행!
            while (_locked.load(std::memory_order_relaxed))
            {
                _mm_pause(); // CPU에게 스핀 루프 중임을 알려 파이프라인 효율화
            }
        }
    }

    void unlock() noexcept
    {
        // Release 메모리 배리어로 임계 영역 내의 모든 메모리 수정을 발행한 뒤 잠금 해제
        _locked.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> _locked{false};
};
```

---

### 1.2 하이브리드 동기화: `CRITICAL_SECTION`, `futex`, `std::mutex`

SpinLock은 임계 영역이 수십 나노초(ns) 이내로 끝날 때는 최고지만, 만약 락을 쥔 스레드가 페이지 폴트(Page Fault)를 겪거나 OS 스케줄러에 의해 코어에서 쫓겨나 버리면(Preempted), 나머지 스레드들이 헛되이 CPU를 100% 태우는 참사가 벌어진다.

이를 해결하기 위해 Windows의 **`CRITICAL_SECTION`**(`InitializeCriticalSectionAndSpinCount`)과 Linux의 **`futex`(Fast Userspace Mutex, `std::mutex`의 기반)**는 **Spin-then-Sleep(적응형 뮤텍스)** 방식을 사용한다.

```text
[하이브리드 Mutex (CRITICAL_SECTION / futex) 동작 흐름]

  lock() 호출
       |
       v
  [유저 모드] CAS로 락 획득 시도 ---> (성공: 경합 없음!) ---> 커널 진입 없이 즉시 리턴! (~15 ns)
       |
  (실패: 다른 스레드가 보유 중)
       |
       v
  [유저 모드] 지정된 횟수(예: 4,000회)만큼 SpinLock처럼 루프를 돌며 대기
       |      ---> (스핀 도중 락 풀림!) ---> Context Switch 없이 즉시 획득!
       |
  (4,000회 스핀 후에도 안 풀림 -> 임계 영역이 길다고 판단)
       |
       v
  [커널 모드 진입] 시스템 콜 호출 -> 커널 대기 큐(Wait Queue)에 스레드 등록 후 Sleep (Context Switch)
```

---

### 1.3 `SRWLock` (Slim Reader/Writer Lock)과 Read-Write Lock

게임 서버의 데이터(예: 길드원 목록, 상점 아이템 테이블, 방 내 유저 목록)는 **수정(Write)은 가끔 일어나지만 조회(Read)는 초당 수만 번 일어나는 경우**가 많다. 이때 일반 `Mutex`를 쓰면 읽기만 하는 스레드끼리도 서로 줄을 서서 기다려야 하는 병목이 발생한다.

#### Read-Write Lock의 동작 규칙
- **Read Lock (Shared Lock, 공유 락)**: 현재 Write Lock을 잡은 스레드가 없다면, **여러 스레드가 동시에 Read Lock을 잡고 진입**할 수 있다.
- **Write Lock (Exclusive Lock, 배타 락)**: 단 하나의 스레드만 진입할 수 있으며, 다른 모든 Read/Write 스레드의 진입을 막는다.

#### Windows `SRWLock`과 C++17 `std::shared_mutex`
- Windows Vista부터 도입된 **`SRWLock` (`RTL_SRWLOCK`)**은 단 **포인터 크기 하나(8바이트)**의 메모리만 차지하는 초경량 커널/유저 하이브리드 Read-Write Lock이다. C++17의 `std::shared_mutex`도 최신 MSVC에서는 내부적으로 `SRWLock`을 래핑하여 구현되어 있다.
- **커스텀 RWLock 구현 기법 (32비트 비트마스크 패킹)**:
  실습에서 직접 RWLock(재귀적 락 허용 포함)을 구현할 때는 32비트 `std::atomic<uint32_t>` 변수 하나를 비트 단위로 쪼개어 상태를 관리하는 기법을 주로 사용한다.

```text
[ 32-bit Atomic RWLock Flag 구조 ]
+-------------------------------+-------------------------------+
|   Write Thread ID (16 Bit)    |      Read Count (16 Bit)      |
| (현재 Write Lock을 쥔 스레드)  | (현재 동시에 읽고 있는 스레드 수)|
+-------------------------------+-------------------------------+
 31                           16 15                            0
```
- **Write Lock 획득**: 상위 16비트가 `0`이고 하위 16비트(Read Count)도 `0`일 때만(즉, 전체 32비트 값이 `0x00000000`일 때만) CAS를 통해 상위 16비트에 내 `ThreadId`를 기록한다.
- **Read Lock 획득**: 상위 16비트(Write Thread ID)가 `0`인지 확인한 뒤, CAS를 통해 하위 16비트(`Read Count`)를 `+1` 증가시킨다.

---

### 1.4 `Semaphore`(세마포어) 비교
- **Mutex**는 **"소유권(Ownership)"**이 있는 잠금 장치다. 오직 락을 잠근(`lock`) 그 스레드만이 락을 풀(`unlock`) 수 있으며, 동시에 1개의 스레드만 진입한다.
- **Semaphore**는 소유권 개념이 없는 **"공유 자원의 개수 카운터(Counter)"**다. 설정된 초기 카운트($N$)만큼의 스레드가 동시에 진입(`Acquire` 시 카운트 감소)할 수 있으며, **락을 획득하지 않은 제3의 스레드가 `Release()`를 호출해 카운트를 증가시키고 대기 중인 스레드를 깨워줄 수도 있다.** (예: DB 커넥션 풀의 가용 커넥션 개수 제한, 또는 워커 스레드 일감 통지용).

---

## 2. Condition Variable과 Spurious Wakeup (가짜 기상)

일감 큐(Job Queue)가 비어있을 때 워커 스레드를 재우고, 새로운 일감이 들어왔을 때 깨우기 위해 **조건 변수(`std::condition_variable`)**를 사용한다.

### 2.1 왜 반드시 `while` 루프로 조건을 다시 검사해야 하는가?

`condition_variable::wait()`를 사용할 때 `if`문으로 조건을 검사하면 심각한 버그가 발생한다. 반드시 **조건절(Predicate)을 포함한 `while` 루프**를 사용해야 한다.

```cpp
std::mutex mtx;
std::condition_variable cv;
std::queue<Job*> jobQueue;

// [잘못된 코드 - 버그 발생!]
void WorkerThread_Bad()
{
    std::unique_lock<std::mutex> lock(mtx);
    if (jobQueue.empty())
        cv.wait(lock); // 깨어났을 때 jobQueue에 정말 데이터가 있을까? 보장 못 함!
    
    Job* job = jobQueue.front(); // Crash! (빈 큐에서 꺼낼 위험)
}

// [올바른 정석 코드]
void WorkerThread_Good()
{
    std::unique_lock<std::mutex> lock(mtx);
    // 람다식을 넘기면 내부적으로 while (jobQueue.empty()) cv.wait(lock); 과 동일하게 동작함!
    cv.wait(lock, []() { return !jobQueue.empty(); });

    Job* job = jobQueue.front();
    jobQueue.pop();
    lock.unlock(); // 락을 빨리 풀어준 뒤 일감 처리

    job->Execute();
}
```

`wait()`에서 깨어났는데도 큐가 비어있을 수 있는 이유는 다음 두 가지다:

1. **Spurious Wakeup (가짜 기상)**:
   - OS 커널(특히 Linux의 `futex` 및 POSIX 스레드 구현)은 시그널(Signal) 인터럽트 처리나 멀티코어 동기화 성능을 위해, **아무도 `notify_one()` / `notify_all()`을 호출하지 않았는데도 대기 중인 스레드가 저절로 깨어나는 현상(Spurious Wakeup)**을 스펙상 허용한다. 만약 이를 커널 단에서 완벽히 막으려면 모든 `wait` 연산의 성능이 크게 떨어지기 때문에, 유저 공간에서 `while`문으로 한 번 더 검사하도록 책임을 위임한 것이다.
2. **도둑맞은 기상 (Stolen Wakeup)**:
   - 생산자 스레드가 큐에 일감 1개를 넣고 `cv.notify_one()`을 호출하여 대기 중이던 **소비자 스레드 A**를 깨웠다.
   - 그러나 스레드 A가 커널 대기 상태에서 깨어나 `mtx` 뮤텍스를 다시 잡으려 하는 그 찰나의 순간에, 밖에서 새로 진입한 **소비자 스레드 B**가 먼저 `mtx` 락을 새치기해서 잡고 일감 1개를 꺼내 가버릴 수 있다.
   - 뒤늦게 `mtx`를 획득한 스레드 A가 일어났을 때는 이미 큐가 다시 텅 비어있게 된다.

---

## 3. 데드락(Deadlock, 교착 상태) 탐지 및 회피

두 개 이상의 스레드가 서로 상대방이 점유하고 있는 락(Lock)이 풀리기만을 무한정 기다리며 서버 전체가 멈춰버리는 현상을 **데드락(Deadlock)**이라 한다.

### 3.1 데드락의 4대 발생 필요충분조건 (Coffman Conditions)

다음 4가지 조건이 **동시에 모두 만족될 때만** 데드락이 발생한다. 즉, 넷 중 **단 하나만 깨뜨려도** 데드락은 절대 발생하지 않는다.

1. **상호 배제 (Mutual Exclusion)**: 자원은 한 번에 한 스레드만 독점 사용할 수 있다.
2. **점유와 대기 (Hold and Wait)**: 락 A를 이미 쥐고(Hold) 있는 상태에서, 이를 놓지 않고 다른 락 B를 추가로 획득하기 위해 기다린다(Wait).
3. **비선점 (No Preemption)**: 다른 스레드가 쥐고 있는 락을 강제로 빼앗을 수 없다.
4. **환형 대기 (Circular Wait)**: 스레드들이 `T1 -> T2 -> T3 -> T1` 형태로 꼬리를 물고 서로의 자원을 기다리는 사이클(Cycle)을 형성한다.

#### 전형적인 게임 서버 데드락 시나리오 (Lock AB-BA 역전)
- **패킷 1 (유저 A가 유저 B를 공격)**: `UserA.Lock()` 획득 $\rightarrow$ 데미지 판정을 위해 `UserB.Lock()` 획득 시도
- **패킷 2 (유저 B가 유저 A를 공격)**: `UserB.Lock()` 획득 $\rightarrow$ 데미지 판정을 위해 `UserA.Lock()` 획득 시도
- 두 패킷이 서로 다른 워커 스레드에서 동시에 실행되면 **100% 데드락**에 빠진다!

---

### 3.2 실무 데드락 회피(Prevention) 전략

현실적으로 1번(상호 배제)과 3번(비선점)은 데이터 무결성 때문에 깨뜨릴 수 없다. 따라서 게임 서버에서는 **4번(환형 대기)**을 깨뜨리거나, 아예 공유 메모리 락 대신 **Actor(Job Queue) 모델**로 구조를 바꾼다.

#### 전략 1: Lock Hierarchy (락 계층 / 락 순서 강제)
**모든 락에 고유한 우선순위(ID)를 부여하고, 항상 작은 번호에서 큰 번호 순서로만 락을 잡도록 강제**하면 수학적으로 환형 대기(Cycle)가 절대 만들어질 수 없다.

- **클래스 간 순서 규칙**: 예: 항상 `GuildManager Lock (1등급)` $\rightarrow$ `Room Lock (2등급)` $\rightarrow$ `Player Lock (3등급)` 순서로만 잡는다. 절대 `Player` 락을 쥔 채로 `Room` 함수를 호출하지 않는다.
- **동일 클래스 인스턴스 간 순서 규칙 (`std::lock` 또는 고유 ID 정렬)**:
  위의 `UserA`와 `UserB`처럼 같은 등급의 객체 두 개를 동시에 잠가야 할 때는 **유저의 고유 ID(`playerId`)가 작은 쪽부터 먼저 잠그도록 정렬**한다.

```cpp
void TradeItems(Player& playerA, Player& playerB)
{
    if (&playerA == &playerB) return;

    // 항상 고유 ID가 작은 플레이어의 락을 먼저 잡도록 순서를 통일! (AB-BA 원천 차단)
    Player& first  = (playerA.GetId() < playerB.GetId()) ? playerA : playerB;
    Player& second = (playerA.GetId() < playerB.GetId()) ? playerB : playerA;

    std::lock_guard<std::mutex> lock1(first.GetMutex());
    std::lock_guard<std::mutex> lock2(second.GetMutex());

    // 안전하게 아이템 교환 수행 (또는 C++17 std::scoped_lock(playerA.mtx, playerB.mtx) 사용)
}
```

---

### 3.3 그래프 탐색(DFS)을 통한 런타임 데드락 탐지기 (`DeadlockProfiler`) 구현

수십 명의 프로그래머가 협업하는 대형 프로젝트에서는 누군가 실수로 락 순서를 어기는 코드를 작성하기 마련이다. 문제는 데드락이 **개발 PC에서는 타이밍이 안 맞아 전혀 안 터지다가, 라이브 서버 점검 직후 유저가 몰릴 때 터진다**는 점이다.

이를 막기 위해 **실제로 데드락이 터지지 않더라도, 단 한 번이라도 위험한 락 획득 순서(Cycle 가능성)가 실행되면 즉시 감지하여 크래시(Assert)를 내주는 런타임 데드락 프로파일러**를 디버그 빌드에 탑재한다.

#### 동작 원리: 방향 그래프(Directed Graph)의 사이클(Cycle) 탐지
1. 각 스레드는 **TLS(Thread Local Storage)**에 자신이 현재 보유 중인 락들의 스택(`thread_local std::stack<int32_t> LLockStack`)을 유지한다.
2. 어떤 스레드가 `Lock A`를 쥐고 있는 상태에서 `Lock B`를 새로 획득할 때마다, 전역 `DeadlockProfiler`의 방향 그래프에 **`A -> B` 간선(Edge)**을 기록한다.
3. 새로운 간선 `A -> B`가 추가되는 순간, 그래프에 **사이클(Cycle, 예: 이전에 다른 스레드에서 `B -> A` 순서로 잡은 적이 있는지)**이 생겼는지 **DFS(깊이 우선 탐색)**로 검사한다.
4. 만약 **역방향 간선(Back Edge)**이 발견되면, 비록 지금은 두 스레드가 시간차로 실행되어 데드락에 안 걸렸더라도 **"언젠가 동시에 실행되면 100% 데드락이 터질 잠재적 버그"**이므로 즉시 호출 스택을 덤프하고 프로세스를 강제 종료시켜 개발자가 즉시 수정하게 만든다.

```text
[DeadlockProfiler 그래프 탐색 원리]

1. 아침 10시: Thread 1이 [RoomLock]을 잡은 채로 [PlayerLock]을 잡음
   -> 그래프에 간선 추가: RoomLock -----> PlayerLock (정상)

2. 오후 3시: Thread 2가 [PlayerLock]을 잡은 채로 [RoomLock]을 잡으려 시도!
   -> 간선 추가 시도: PlayerLock -----> RoomLock
   -> DFS 탐색 결과: RoomLock -> PlayerLock -> RoomLock (Back Edge / Cycle 발견!)
   -> 실제로 데드락에 걸리지 않았어도 즉시 CRASH("DEADLOCK_DETECTED") 발생!
```

#### DFS에서 사이클(Back Edge)을 판별하는 핵심 로직
방향 그래프(Directed Graph)에서 단순히 `visited[there] == true`라고 해서 무조건 사이클인 것은 아니다(**교차 간선 Cross Edge**나 **순방향 간선 Forward Edge**일 수 있음).
- **진짜 사이클(Back Edge)의 조건**: 내가 지금 탐색을 시작한 경로가 **아직 끝나지 않은 상태(`finished[there] == false`)**인데, 이미 방문했던 조상 노드(`discovered[there] != -1`)를 다시 만났을 때만 사이클이다!

```cpp
// DeadlockProfiler의 핵심 DFS 사이클 검사 의사코드
void DeadlockProfiler::Dfs(int32_t here)
{
    if (_discoveredOrder[here] != -1)
        return;

    _discoveredOrder[here] = _visitCount++;

    // here 락을 잡은 상태에서 이어서 잡았던 모든 인접 락(there)들을 순회
    for (int32_t there : _lockHistory[here])
    {
        // 아직 방문한 적 없는 정점이면 재귀 탐색
        if (_discoveredOrder[there] == -1)
        {
            _parent[there] = here;
            Dfs(there);
            continue;
        }

        // here가 there보다 먼저 발견되었다면 순방향 간선(Forward Edge)이므로 안전함
        if (_discoveredOrder[here] < _discoveredOrder[there])
            continue;

        // [핵심] 나보다 먼저 발견된 정점(there)인데, 아직 there의 DFS가 끝나지 않았다면?
        // 즉, there가 나의 현재 호출 스택 상의 조상(Ancestor)이라면 -> 역방향 간선(Back Edge = Cycle)!
        if (_finished[there] == false)
        {
            PrintCyclePath(here, there);
            std::abort(); // 잠재적 데드락 즉시 검거!
        }
    }

    _finished[here] = true;
}
```

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **SpinLock vs Mutex**: SpinLock은 Context Switch 비용을 아끼기 위해 유저 모드에서 루프를 돌며 대기하므로 **임계 영역이 극도로 짧고 경합이 적을 때**만 유리하다. 구현 시 캐시 버스 폭풍을 막기 위해 읽기(`load`)로 먼저 대기하는 **TTAS 패턴**과 `_mm_pause()`를 적용해야 한다.
2. **하이브리드 동기화 (`CRITICAL_SECTION`, `futex`)**: 경합이 없을 때는 유저 모드 CAS 연산 1회로 빠르게 통과하고, 경합 발생 시 일정 횟수 스핀한 뒤에도 안 풀릴 때만 커널 모드로 진입해 스레드를 블로킹한다.
3. **Spurious Wakeup**: `condition_variable::wait()`는 OS 시그널이나 다른 스레드의 새치기(Stolen Wakeup)로 인해 조건이 충족되지 않았는데도 깨어날 수 있으므로, **반드시 `while` 루프(또는 조건 람다식)로 조건을 재검사**해야 한다.
4. **데드락 해결과 탐지**: 실무에서는 4대 조건 중 **환형 대기(Circular Wait)**를 깨기 위해 **Lock Hierarchy(락 순서 강제)**를 적용하며, 개발 단계에서는 락 획득 이력을 방향 그래프로 그려 **DFS 역방향 간선(Back Edge)을 탐지하는 `DeadlockProfiler`**로 잠재적 데드락을 사전에 박멸한다.
