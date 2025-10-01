#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <map>
#include <random>
#include <chrono>
#include <cmath>
#include <tuple>
#include <string>
#include <cstdio> // for setvbuf

#ifdef _WIN32
#include <windows.h>
void EnableVTMode() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}
#endif

// ---------------- Constants ----------------
const int GRID_SIZE = 11; // -5 to +5
const int HISTORY_LIMIT = 50; // max number of actions to display

struct GameAction {
    int clientID;
    std::string type;
    float dx, dy, dz;
    int gx, gy;
    bool illegal = false;
    GameAction() : dx(0), dy(0), dz(0), gx(0), gy(0), illegal(false) {}
};

// ---------------- Thread-safe queue ----------------
template<typename T>
class TSQueue {
    std::queue<T> q;
    std::mutex mtx;
public:
    void push(const T& item) { std::lock_guard<std::mutex> lock(mtx); q.push(item); }
    bool try_pop(T& out) { std::lock_guard<std::mutex> lock(mtx); if (q.empty()) return false; out = q.front(); q.pop(); return true; }
};

// ---------------- Random utility ----------------
float getRandomFloat(float min, float max) {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<float> dist(min, max);
    return dist(gen);
}

// ---------------- Shared resources ----------------
TSQueue<GameAction> actionQueue;
std::mutex stateMutex;
std::map<int, std::tuple<float, float, float>> serverState;
std::map<int, std::tuple<float, float, float>> clientPredicted;
std::vector<GameAction> actionHistory;
std::map<int, int> penalties;
bool done = false;

// ---------------- Validation ----------------
bool validateAction(GameAction& a) {
    float x = std::get<0>(serverState[a.clientID]);
    float y = std::get<1>(serverState[a.clientID]);

    float nx = x, ny = y;
    if (a.type == "Move") { nx += a.dx; ny += a.dy; }

    int gx = std::round(nx) + 5;
    int gy = GRID_SIZE - 1 - (std::round(ny) + 5);
    a.gx = gx; a.gy = gy;

    if (nx < -5.0f || nx>5.0f || ny < -5.0f || ny>5.0f) {
        a.illegal = true;
        return false;
    }
    return true;
}

// ---------------- ANSI colors ----------------
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN = "\033[36m";
const std::string GRAY = "\033[90m"; // oldest actions

// ---------------- Server ----------------
void serverThread(const std::vector<int>& clientIDs, int latencyMs = 100) {
    std::cout << "\033[?25l"; // hide cursor

    while (!done) {
        GameAction action;
        while (actionQueue.try_pop(action)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(latencyMs));

            std::lock_guard<std::mutex> lock(stateMutex);
            bool legal = validateAction(action);

            if (legal) {
                auto& pos = serverState[action.clientID];
                float x = std::get<0>(pos);
                float y = std::get<1>(pos);
                if (action.type == "Move") { x += action.dx; y += action.dy; }
                pos = std::make_tuple(x, y, 0);
            }
            else {
                penalties[action.clientID]++;
            }

            // Add to history
            actionHistory.push_back(action);
            if (actionHistory.size() > HISTORY_LIMIT) actionHistory.erase(actionHistory.begin());

            // Update client predicted positions
            for (int id : clientIDs) clientPredicted[id] = serverState[id];
        }

        // Draw ASCII grid
        std::vector<std::string> grid(GRID_SIZE * GRID_SIZE, ".");

        {
            int historySize = actionHistory.size();
            for (int i = 0; i < historySize; i++) {
                auto& act = actionHistory[i];
                int idx = act.gy * GRID_SIZE + act.gx;
                if (idx < 0 || idx >= GRID_SIZE * GRID_SIZE) continue;

                // Determine fade level
                float age = float(historySize - i) / historySize;
                std::string color;
                if (act.illegal) color = MAGENTA;
                else if (act.type == "Move") color = GREEN;
                else if (act.type == "Jump") color = YELLOW;
                else if (act.type == "Shoot") color = RED;

                if (age < 0.33f) color = GRAY;       // oldest
                else if (age < 0.66f) color = "\033[2m" + color; // medium dim
                // else bright for recent

                if (act.illegal) grid[idx] = color + "X" + RESET;
                else if (act.type == "Move") grid[idx] = color + "M" + RESET;
                else if (act.type == "Jump") grid[idx] = color + "J" + RESET;
                else if (act.type == "Shoot") grid[idx] = color + "S" + RESET;
            }

            // Highlight current client positions
            for (auto& kv : clientPredicted) {
                int id = kv.first;
                float fx = std::get<0>(kv.second);
                float fy = std::get<1>(kv.second);
                int gx = std::round(fx) + 5;
                int gy = GRID_SIZE - 1 - (std::round(fy) + 5);
                int idx = gy * GRID_SIZE + gx;
                if (idx >= 0 && idx < GRID_SIZE * GRID_SIZE) {
                    grid[idx] = CYAN + std::to_string(id) + RESET;
                }
            }
        }

        // Move cursor to top-left
        std::cout << "\033[H";
        std::cout << "=== ASCII Game Map (Live) ===\n";
        for (int y = 0; y < GRID_SIZE; y++) {
            for (int x = 0; x < GRID_SIZE; x++) {
                std::cout << grid[y * GRID_SIZE + x] << " ";
            }
            std::cout << "\n";
        }
        std::cout << "\nPenalties: ";
        for (auto& p : penalties) std::cout << "Client " << p.first << "=" << p.second << " ";
        std::cout << "\n" << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    std::cout << "\033[?25h"; // show cursor
}

// ---------------- Client ----------------
void clientThread(int id, int latencyMs = 50) {
    serverState[id] = std::make_tuple(0.0f, 0.0f, 0.0f);
    clientPredicted[id] = std::make_tuple(0.0f, 0.0f, 0.0f);

    std::vector<std::string> actions = { "Move","Jump","Shoot" };
    while (!done) {
        GameAction a;
        a.clientID = id;
        a.type = actions[int(getRandomFloat(0.0f, 3.0f)) % 3];
        if (a.type == "Move") { a.dx = getRandomFloat(-1.0f, 1.0f); a.dy = getRandomFloat(-1.0f, 1.0f); }
        else a.dz = getRandomFloat(-3.0f, 3.0f);

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            auto& pred = clientPredicted[id];
            float x = std::get<0>(pred);
            float y = std::get<1>(pred);
            float z = std::get<2>(pred);
            if (a.type == "Move") { x += a.dx; y += a.dy; }
            else z = a.dz;
            pred = std::make_tuple(x, y, z);
        }

        actionQueue.push(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(latencyMs));
    }
}

// ---------------- Main ----------------
int main() {
    // Disable buffering for live output
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    EnableVTMode();
#endif

    const int numClients = 2;
    std::vector<int> clientIDs;
    std::vector<std::thread> clients;
    for (int i = 1; i <= numClients; i++) clientIDs.push_back(i);

    std::thread server(serverThread, clientIDs, 50);
    for (int id : clientIDs) clients.emplace_back(clientThread, id, 50);

    std::this_thread::sleep_for(std::chrono::seconds(15));
    done = true;

    for (auto& c : clients) c.join();
    server.join();

    std::cout << "\nFinal penalties:\n";
    for (auto& p : penalties) std::cout << "Client " << p.first << "=" << p.second << "\n";
    std::cout << "Simulation finished.\n";
    return 0;
}
