#include <memory>
#include <functional>
#include <vector>
#include <map>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <array>
#include <set>
#include <random>
#include <chrono>
#include <iostream>
#include <algorithm>

#include "hlt.hpp"
#include "networking.hpp"
//#include "socket_networking.hpp"

//#define DEBUG
#define FULLDEBUG 0
bool active = false;

class Timer {
public:
	Timer() : m_startTime(), m_timeOut(), m_started(false) {};

	void startTimer(double durationInMilliseconds) {
		m_startTime = std::chrono::high_resolution_clock::now();
		m_timeOut = std::chrono::nanoseconds((long long)(durationInMilliseconds * 1000000));
		m_started = true;
	};

	bool timeCheck() const {
		if (m_started) {
			std::chrono::nanoseconds timeSpent = std::chrono::high_resolution_clock::now() - m_startTime;
			if (timeSpent > m_timeOut) {
				return true;
			}
		}
		return false;
	}

	std::chrono::milliseconds currentTimeTakenInMilliSeconds() const {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_startTime);
	}

	friend std::ostream& operator<<(std::ostream &os, const Timer& timer) {
		os << timer.currentTimeTakenInMilliSeconds().count() << "ms";
		return os;
	}

private:
	std::chrono::high_resolution_clock::time_point m_startTime;
	std::chrono::nanoseconds m_timeOut;
	bool m_started;
};

class Tile {
public:
	unsigned char x, y;
	unsigned short id;

	unsigned char owner;
	unsigned char strength;
	unsigned char production;
	char move; // default -1

	float sDivP; // strength / production
	std::array<Tile*, 4> neighbours;
	short used; // id of global path (one Tile can be associated with many paths but its strength and production can only be used once)

	Tile() : x(0), y(0), id(0), owner(0), strength(0), production(0), move(-1), sDivP(0), used(-1) {
	}
	Tile(unsigned char x, unsigned char y, unsigned char o, unsigned char s, unsigned char p, unsigned char width) :
		x(x), y(y),
		id(y*width + x),
		owner(o),
		strength(s),
		production(p),
		move(-1),
		used(-1)
	{
		sDivP = production == 0 ? 1000 : float(strength) / production;
	}

	bool operator==(const Tile& t) const  {
		return id == t.id;
	}
	
	void update(signed char o, unsigned char s) {
		owner = o;
		strength = s;
		sDivP = production == 0 ? 1000 : float(strength) / production;
		move = -1;
		used = -1;
	}
	inline unsigned short cost() {
		return production;
	}

	friend std::ostream& operator<<(std::ostream& os, const Tile& tile) {
		os << std::setw(4) << tile.id << " x:" << std::setw(2) << (int)tile.x << " y:" << std::setw(2) << (int)tile.y << " o:"
			<< (int)tile.owner << " s:" << std::setw(3) << (int)tile.strength << " p:" << std::setw(3) << (int)tile.production
			<< " m:" << std::setw(2) << (int)tile.move << " " << " u:" << std::setw(2) << (int)tile.used;
			// << " " << std::setw(5) << std::fixed << std::setprecision(2) << tile.sDivP;
		return os;
	}
};
class TileChanged {
public:
	Tile* ref;
	unsigned char changed; // new own tile ... 1, removed tile ... 2

	TileChanged(Tile& tile, unsigned char c) : ref(&tile), changed(c) {}

	friend std::ostream& operator<<(std::ostream& os, const TileChanged& tile) {
		os << std::setw(4) << tile.ref->id << " changed:" << std::setw(4) << (tile.changed==1?" new":" cha") << " o:" << (int)tile.ref->owner
			<< " s:" << std::setw(3) << (int)tile.ref->strength << " p:" << std::setw(3) << (int)tile.ref->production << " "
			<< std::setw(5) << std::fixed << std::setprecision(2) << tile.ref->sDivP;
		return os;
	}
};
class AdjacentTile {
private:
	unsigned short getPathProduction() {
		unsigned short prod = 0;
		for (size_t i = 1; i < m_path.size() - 1; i++) {
			prod += m_path[i]->production;
		}
		return prod;
	}
public:
	Tile* m_start;
	Tile* m_target;
	unsigned short m_dist;
	std::vector<Tile*> m_path;
	float m_value;

	AdjacentTile() : m_start(nullptr), m_target(nullptr), m_dist(-1), m_value(-1) {}
	AdjacentTile(Tile* start, Tile* target, unsigned short dist, std::vector<Tile*>& path, float penalty, unsigned char id) : m_start(start), m_target(target), m_dist(dist), m_path(path){
		unsigned short sum = getPathProduction();
		m_value = m_target->strength + sum + penalty * (std::max)((unsigned int)0, (unsigned int)(path.size()-2));
		// only incoporate enemy tiles if next to
		// global best and local best
		if (m_path.size() == 2) {
			unsigned short damage = 0;
			for (Tile* t : m_target->neighbours) {
				if (t->owner != 0 && t->owner != id) {
					damage += (std::min)(t->strength, m_start->strength);
				}
			}
			m_value -= damage;
		}
		m_value /= m_target->production;
	}

	bool operator<(const AdjacentTile& t) const {
		if (m_value != t.m_value) {
			return m_value < t.m_value;
		} else {
			if (m_target->production != t.m_target->production) {
				return m_target->production > t.m_target->production;
			} else {
				return m_dist < t.m_dist;
			}
		}
	}

	friend std::ostream& operator<<(std::ostream& os, const AdjacentTile& a) {
		os << *(a.m_target) << " dist:" << std::setw(4) << (int)a.m_dist << " value:" << std::setw(5) << std::fixed << std::setprecision(2) << a.m_value << " path:";
		for (Tile* t : a.m_path) {
			os << t->id << "|";
		}
		return os;
	}
};
class DijkstraSearch {
private:
	std::vector<unsigned short> distMap;
#if FULLDEBUG
	std::map<unsigned short, Tile*> cameFrom;
	std::map<unsigned short, unsigned short> costSoFar;
	std::map<unsigned short, Tile*> adjacentTiles;
#else
	std::unordered_map<unsigned short, Tile*> cameFrom;
	std::unordered_map<unsigned short, unsigned short> costSoFar;
	std::unordered_map<unsigned short, Tile*> adjacentTiles;
#endif
	// save dist, adjacent tiles, cameFrom
	// adjacentTiles: there may be islands, so for each start tile
	void dijkstra(Tile* start, unsigned char id) {
		// init queue
		std::priority_queue<std::pair<unsigned short, Tile*>, std::vector<std::pair<unsigned short, Tile*>>, std::greater<std::pair<unsigned short, Tile*>>> q;
		q.emplace(std::make_pair(0, start));
		// init cameFrom
		cameFrom[start->id] = start;
		// init cost_so_far
		costSoFar[start->id] = 0;
		// init dist
		distMap[start->id] = 0;

		for (; !q.empty();) {
			Tile* zone = q.top().second;
			q.pop();

			if (zone->owner == id) {
				for (size_t i = 0; i < 4; i++) {
					Tile* next = zone->neighbours[i];
					unsigned short new_cost = costSoFar[zone->id] + next->cost();
					unsigned short new_dist = distMap[zone->id] + 1;
					if (!costSoFar.count(next->id) || new_cost < costSoFar[next->id] || (new_cost == costSoFar[next->id] && new_dist < distMap[next->id])) {
						costSoFar[next->id] = new_cost;
						cameFrom[next->id] = zone;
						distMap[next->id] = new_dist;
						if (next->owner == id) {
							q.emplace(std::make_pair(new_cost, next));
						} else {
							if (!adjacentTiles.count(next->id)) {
								adjacentTiles[next->id] = next;
							}
						}
					}
				}
			}
		}
	}
	// from start tile to target tile
	// at least 2 tiles
	bool reconstructPath(Tile* target, std::vector<Tile*>& path) {
		path.reserve(64);
		Tile* current = target;
		path.push_back(current);
		while (!(*current == *start)) {
			try {
				current = cameFrom.at(current->id);
			}
			catch (...) {
				path.clear();
				return false;
			}
			path.push_back(current);
		}
		std::reverse(path.begin(), path.end());
		return true;
	}
public:
	Tile* start;
	DijkstraSearch() {
		start = nullptr;
	}
	DijkstraSearch(Tile* s, std::vector< std::vector<Tile> >& gameMap, unsigned char width, unsigned char height, unsigned char id) : distMap(height*width, -1) {
		start = s;

		dijkstra(start, id);
	}

	std::vector<AdjacentTile> getAdjacentTiles(float penalty, unsigned char id, bool debug, std::ostream& out) {
		std::vector<AdjacentTile> temp;
		temp.reserve(adjacentTiles.size());
		for (const std::pair<unsigned short, Tile*>& t : adjacentTiles) {
			std::vector<Tile*> path;
			bool reconstructed = reconstructPath(t.second, path);
			if (!reconstructed) {
				continue;
			}
			temp.push_back(AdjacentTile(start, t.second, distMap[t.first], path, penalty, id));
		}

		sort(temp.begin(), temp.end());

		if (debug && FULLDEBUG) {
			out << "adjacent tiles: " << std::endl;
			for (const auto& t : temp) {
				out << t << std::endl;
			}
		}

		// remove all tiles with strength > 0 && owner == 0 and enemy neighbours
		temp.erase(std::remove_if(temp.begin(), temp.end(), [&id](const AdjacentTile& x) {
			if (x.m_target->strength > 0 && x.m_target->owner == 0) {
				for (Tile* t : x.m_target->neighbours) {
					if (t->owner != id && t->owner != 0) {
						return true;
					}
				}
			}
			return false;
		}), temp.end());

		return temp;
	}
	void dijkstraContinue(std::vector<TileChanged> changedTiles, unsigned char id) {
		// new tiles: find neighbour with min distance and insert this tile into the queue
		// removed tiles: clean up all paths in cameFrom (and costSoFar) starting from this tile

		// queue
		std::priority_queue<std::pair<unsigned short, Tile*>, std::vector<std::pair<unsigned short, Tile*>>, std::greater<std::pair<unsigned short, Tile*>>> q;

		sort(changedTiles.begin(), changedTiles.end(), [](const TileChanged& a, const TileChanged& b) {
			return a.changed > b.changed;
		});

		std::unordered_set<Tile*> checkIds;
		for (const TileChanged& tc : changedTiles) {
			if (tc.changed == 1) { // new
				unsigned short dist = -1;
				for (Tile* n : tc.ref->neighbours) {
					if (n->owner == id) {
						checkIds.insert(n);
					}
				}
				costSoFar.erase(tc.ref->id);
				distMap[tc.ref->id] = -1;
				cameFrom.erase(tc.ref->id);
				adjacentTiles.erase(tc.ref->id);
			} else if (tc.changed == 2) { // removed
				std::vector<Tile*> removeTiles;
				removeTiles.reserve(8);
				removeTiles.push_back(tc.ref);
				while (!removeTiles.empty()) {
					unsigned short removeId = removeTiles[0]->id;
					costSoFar.erase(removeId);
					distMap[removeId] = -1;
					cameFrom.erase(removeId);
					adjacentTiles.erase(removeId);

					// queue all neighbours which are own tiles
					for (Tile* n : removeTiles[0]->neighbours) {
						if (n->owner == id) {
							checkIds.insert(n);
						}
					}
					for (const auto& p : cameFrom) {
						if (p.second->id == removeId) {
							for (Tile* n : removeTiles[0]->neighbours) {
								if (n->id == p.first) {
									removeTiles.push_back(n);
									break;
								}
							}
						}
					}
					removeTiles.erase(removeTiles.begin());
				}
			}
		}

		for (Tile* n : checkIds) {
			if (costSoFar.count(n->id)) {
				q.emplace(std::make_pair(costSoFar[n->id], n));
			}
		}

		// identical, see above
		for (; !q.empty();) {
			Tile* zone = q.top().second;
			q.pop();

			if (zone->owner == id) {
				for (size_t i = 0; i < 4; i++) {
					Tile* next = zone->neighbours[i];
					unsigned short new_cost = costSoFar[zone->id] + next->cost();
					unsigned short new_dist = distMap[zone->id] + 1;
					if (!costSoFar.count(next->id) || new_cost < costSoFar[next->id] || (new_cost == costSoFar[next->id] && new_dist < distMap[next->id])) {
						costSoFar[next->id] = new_cost;
						cameFrom[next->id] = zone;
						distMap[next->id] = new_dist;
						if (next->owner == id) {
							q.emplace(std::make_pair(new_cost, next));
						} else {
							if (!adjacentTiles.count(next->id)) {
								adjacentTiles[next->id] = next;
							}
						}
					}
				}
			}
		}
	}
	bool isIdentical(const DijkstraSearch& other, bool debug, std::ostream& out) {
		if (start->id != other.start->id) {
			if (debug && FULLDEBUG) out << "Not identical: start failed!" << std::endl;
			return false;
		}
		if (costSoFar != other.costSoFar) {
			if (debug && FULLDEBUG) {
				auto it2 = other.costSoFar.begin();
#if FULLDEBUG
				for (std::map<unsigned short, unsigned short>::iterator it1 = costSoFar.begin(); it1 != costSoFar.end(); ++it1, ++it2) {
#else
				for (std::unordered_map<unsigned short, unsigned short>::iterator it1 = costSoFar.begin(); it1 != costSoFar.end(); ++it1, ++it2) {
#endif
					out << it1->first << " => " << it1->second << " | " << it2->first << " => " << it2->second << std::endl;
				}
				out << "Not identical: costSoFar failed!" << std::endl;
			}
			return false;
		}
		if (distMap != other.distMap) {
			if (debug && FULLDEBUG) out << "Not identical: distMap failed!" << std::endl;
			return false;
		}
		if (!std::equal(cameFrom.begin(), cameFrom.end(), other.cameFrom.begin(), [](const std::pair<unsigned short, Tile*>& lhs, const std::pair<unsigned short, Tile*>& rhs) { return *(lhs.second) == *(rhs.second); })) {
			if (debug && FULLDEBUG) out << "Not identical: cameFrom failed!" << std::endl;
			return false;
		}
		if (!std::equal(adjacentTiles.begin(), adjacentTiles.end(), other.adjacentTiles.begin(), [](const std::pair<unsigned short, Tile*>& lhs, const std::pair<unsigned short, Tile*>& rhs) { return *(lhs.second) == *(rhs.second); })) {
			if (debug && FULLDEBUG) out << "Not identical: adjacentTiles failed!" << std::endl;
			return false;
		}

		return true;
	}
	bool checkExpansion() {
		for (const std::pair<unsigned short, Tile*>& t : adjacentTiles) {
			if (t.second->owner == 0 && t.second->strength == 0) {
				return false;
			}
		}
		return true;
	}
};

class PathSearch {
private:
	inline bool canBeUsed(Tile* t, unsigned char turn, unsigned char moves) {
		// use tile if a) not used 
		// b) smaller turns or c) equal turns and smaller moves
		if (t->used == -1 ||
			turn < m_paths[0][t->used].m_turns ||
			turn == m_paths[0][t->used].m_turns && (moves < m_paths[0][t->used].m_moves)) {
			return true;
		} else {
			return false;
		}
	}
public:
	Tile* m_start;
	AdjacentTile m_target;
	unsigned char m_turns; // turns until the target is conquered, at least m_moves
	unsigned char m_length; // how many tiles are included (not all tiles must be relevant), at least 1, maximum = path.size()-1
	unsigned char m_moves; // how many moves until the target is reached = path.size()-1
	std::vector<PathSearch>* m_paths;

	PathSearch() :m_start(nullptr), m_paths(nullptr) {
	}
	PathSearch(Tile* start, AdjacentTile target, std::vector<PathSearch>& paths) : m_start(start), m_target(target), m_paths(&paths) {
		m_moves = target.m_path.size() - 1;

		bool finished = false;
		for (size_t wait = 0; wait < 256; wait++) {
			unsigned short pathStrength = 0;

			size_t k = wait;
			for (m_length = 0; m_length < m_moves; m_length++, k++) {
				Tile* t = target.m_path[m_length];
				if (m_length == 0) { // always use the first tile
					pathStrength += t->strength + (unsigned short)k * t->production;
				} else {
					if (!canBeUsed(t, m_moves + wait, m_moves)) break;
					pathStrength += t->strength + (unsigned short)k * t->production;
				}

				if (pathStrength > m_target.m_target->strength) {
					m_length++;
					m_turns = m_moves + wait;
					finished = true;
					break;
				}
			}


			if (finished) break;
		}
	}

	PathSearch(const PathSearch& other) : m_start(other.m_start), m_target(other.m_target),
		m_paths(other.m_paths), m_turns(other.m_turns), m_length(other.m_length), m_moves(other.m_moves)
	{
	}
	void swap(PathSearch& other) {
		std::swap(m_start, other.m_start);
		std::swap(m_target, other.m_target);
		std::swap(m_length, other.m_length);
		std::swap(m_turns, other.m_turns);
		std::swap(m_moves, other.m_moves);
		std::swap(m_paths, other.m_paths);
	}
	PathSearch& operator=(PathSearch other) {
		swap(other);

		// by convention, always return *this
		return *this;
	}

	bool update(std::vector<Tile*>& released, bool debug, std::ostream& out) {
		if (canBeUsed(m_target.m_path[0], m_turns, m_moves)) {
			size_t insertId = m_paths[0].size();
			released.reserve(m_length);

			// check paths to release
			for (size_t i = 0; i < m_length; i++) {
				Tile* t = m_target.m_path[i];

				if (debug && FULLDEBUG) {
					out << i << "| " << *t << std::endl;
				}

				if (t->used != -1) {
					unsigned short releaseId = t->used;
					for (size_t j = 0; j < m_paths[0][releaseId].m_length; j++) {
						Tile* r = m_paths[0][releaseId].m_target.m_path[j];
						r->used = -1;
						r->move = -1;
						released.push_back(r);
					}
					m_paths[0][releaseId] = PathSearch();
				}

				t->used = insertId;
				t->move = STILL;
			}

			return true;
		}

		return false;
	}

	void print(std::ostream& out) {
		if (m_start == nullptr) {
			out << "empty" << std::endl;
		} else {
			out << "start:" << *m_start << " target:"  << " t:" << (int)m_turns << " l:" << (int)m_length << " m:" << (int)m_moves << " " << m_target << std::endl;
		}
	}
};

class GameState {
public:
	std::vector< std::vector<Tile> > m_gameMap;
	unsigned char m_height;
	unsigned char m_width;
	unsigned char m_id;
	unsigned char m_initialPlayers;
	float m_movePenalty;
	bool m_expansion;
	unsigned int m_territorySize[7] = { 0 };
	Timer m_timer;
	std::vector<Tile*> m_ownTiles;
	std::vector<DijkstraSearch> m_djikstraSearch;
	std::vector<PathSearch> m_paths; // global paths, one Tile can be a path alone

	GameState(const hlt::GameMap& gameMap, unsigned char myId) : m_width((unsigned char)gameMap.width), m_height((unsigned char)gameMap.height), m_id(myId), m_timer(),
		m_expansion(true), m_djikstraSearch(m_height*m_width, DijkstraSearch())
	{
		m_timer.startTimer(950);
		m_gameMap = std::vector< std::vector<Tile> >(m_height, std::vector<Tile>(m_width));
		for (unsigned char y = 0; y < m_height; y++) {
			for (unsigned char x = 0; x < m_width; x++) {
				const hlt::Site& s = gameMap.contents[y][x];
				m_gameMap[y][x] = Tile(x, y, (unsigned char)s.owner, (unsigned char)s.strength, (unsigned char)s.production, m_width);

				for (size_t i = 0; i < 4; i++) {
					m_gameMap[y][x].neighbours[i] = getTile(&m_gameMap[y][x], CARDINALS[i]);
				}
			}
		}
		computeTerritorySize();
		m_initialPlayers = computePlayers();
		m_ownTiles = getPlayerTiles(m_id);

		for (Tile* t : m_ownTiles) {
			m_djikstraSearch[t->id] = DijkstraSearch(t, m_gameMap, m_width, m_height, m_id);
		}
	}
	void computeTerritorySize() {
		for (size_t i = 0; i < 7; i++) {
			m_territorySize[i] = 0;
		}

		for (unsigned short i = 0; i < m_height; i++) {
			for (unsigned short j = 0; j < m_width; j++) {
				m_territorySize[m_gameMap[i][j].owner] += 1;
			}
		}
	}
	unsigned char computePlayers() {
		unsigned char num = 0;
		for (size_t i = 0; i < 7; i++) {
			if (m_territorySize[i]) {
				num += 1;
			}
		}
		return num;
	}
	float getMovePenalty() {
		float penalty = 0;
		for (Tile* t : m_ownTiles) {
			penalty += t->production;
		}
		return penalty / (float)m_ownTiles.size();
	}
	void updateGameState() {
		computeTerritorySize();
		computePlayers();
	}
	void updateGameMap(const hlt::GameMap& gameMap, bool debug = false, std::ostream& out = std::cout) {
		m_timer.startTimer(950);
		std::vector<TileChanged> changedTiles;
		for (unsigned char y = 0; y < m_height; y++) {
			for (unsigned char x = 0; x < m_width; x++) {
				const hlt::Site& s = gameMap.contents[y][x];
				// check new or removed tiles
				if (s.owner != m_gameMap[y][x].owner && (m_gameMap[y][x].owner == m_id || m_id == s.owner)) {
					unsigned char c = 0;
					if (m_id == s.owner) {
						c = 1;
					} else {
						c = 2;
					}
					changedTiles.push_back(TileChanged(m_gameMap[y][x], c));
				}
				m_gameMap[y][x].update((unsigned char)s.owner, (unsigned char)s.strength);
			}
		}
		updateGameState();
		m_ownTiles = getPlayerTiles(m_id);

		if (debug && FULLDEBUG) {
			printMap(out);
			printOwnTiles(out);

			out << "new/removed files:" << std::endl;
			for (const TileChanged& tc : changedTiles) {
				out << tc << std::endl;
			}
		}

		for (Tile* t : m_ownTiles) {
			bool newTile = false;
			for (const TileChanged& tc : changedTiles) {
				if (tc.ref->id == t->id) {
					newTile = true;
				}
			}

			if (newTile) {
				m_djikstraSearch[t->id] = DijkstraSearch(t, m_gameMap, m_width, m_height, m_id);
			} else {
				m_djikstraSearch[t->id].dijkstraContinue(changedTiles, m_id);
			}
		}

		if (debug && FULLDEBUG) {
			std::vector<DijkstraSearch> m_djikstraSearchTemp(m_height*m_width, DijkstraSearch());
			for (Tile* t : m_ownTiles) {
				m_djikstraSearchTemp[t->id] = DijkstraSearch(t, m_gameMap, m_width, m_height, m_id);

				const bool ident = m_djikstraSearchTemp[t->id].isIdentical(m_djikstraSearch[t->id], debug, out);

				if (!ident) {
					out << "Differences in djikstra search with start tile id = " << t->id << std::endl;
				}
			}
		}

		m_paths.clear();
		m_paths.reserve(m_ownTiles.size());
		m_movePenalty = getMovePenalty();

		// check expansion
		if (m_expansion) {
			for (Tile* t : m_ownTiles) {
				m_expansion = m_djikstraSearch[t->id].checkExpansion();
				if (!m_expansion) break;
			}
		}

		if (debug) out << "expansion: " << m_expansion << " penalty: " << m_movePenalty << " Init: " << m_timer << std::endl;
	}

	// Tile functions

	Tile* getTile(Tile* t, unsigned char direction = STILL) {
		unsigned char x = t->x, y = t->y;
		if (direction != STILL) {
			if (direction == NORTH) {
				if (y == 0) y = m_height - 1;
				else y--;
			} else if (direction == EAST) {
				if (x == m_width - 1) x = 0;
				else x++;
			} else if (direction == SOUTH) {
				if (y == m_height - 1) y = 0;
				else y++;
			} else if (direction == WEST) {
				if (x == 0) x = m_width - 1;
				else x--;
			}
		}
		return &m_gameMap[y][x];
	}

	// player functions

	// sorted by id asc
	std::vector<Tile*> getPlayerTiles(unsigned char id) {
		std::vector<Tile*> tiles(0);
		size_t k = 0;
		for (unsigned short y = 0; y < m_height; y++) {
			for (unsigned short x = 0; x < m_width; x++) {
				if (m_gameMap[y][x].owner == id) {
					tiles.push_back(&m_gameMap[y][x]);
				}
			}
		}

		sort(tiles.begin(), tiles.end(), [](Tile* a, Tile* b) {
			return a->id < b->id;
		});

		return tiles;
	}

	// sorted by strength desc
	std::vector<Tile*> getOwnBoarderTiles(unsigned char id) {
		std::vector<Tile*> tiles(0);
		for (Tile* t : m_ownTiles) {
			for (unsigned char c : CARDINALS) {
				Tile* n = getTile(t, c);
				if (n->owner != m_id) {
					tiles.push_back(t);
				}
			}
		}

		sort(tiles.begin(), tiles.end(), [](Tile* a, Tile* b) -> bool {
			return a->strength > b->strength;
		});
	}

	// own functions

	void setMoveDirection(Tile* t, Tile* next) {
		for (unsigned char c : CARDINALS) {
			Tile* n = getTile(t, c);
			if (n->id == next->id) {
				t->move = c;
				break;
			}
		}
	}
	void setMoveForZeroStrengthTiles(std::vector<Tile*>& tiles) {
		for (Tile* t : tiles) {
			if (t->strength == 0) {
				t->move = 0;
			}
		}
	}
	void setMoveForSmallStrengthTiles(std::vector<Tile*>& tiles, unsigned char multi) {
		for (Tile* t : tiles) {
			if (t->strength <= multi*t->production) {
				t->move = 0;
			}
		}
	}

	unsigned char getOppositeDirection(unsigned char d) {
		if (d == 1 || d == 2) {
			return d + 2;
		} else {
			return d - 2;
		}
	}
	AdjacentTile getBestAdjacentTile(std::vector<AdjacentTile>& adjacentTiles) {
		size_t i = 0, mi = 0, over = -1;

		// check cap limit for next move
		for (const AdjacentTile& t : adjacentTiles) {
			Tile* s = t.m_path[0];
			Tile* n = t.m_path[1];

			// check multiple tile moves
			unsigned short sum = s->strength;
			for (unsigned char c : CARDINALS) {
				Tile* o = getTile(n, c);
				if (o->owner == m_id && s->id != o->id && o->move == getOppositeDirection(c)) {
					sum += o->strength;
				}
			}

			if (n->owner == m_id && (n->move == -1 || n->move == 0) && sum + n->strength + n->production > 255) {
				if (sum + n->strength + n->production - 255 < over) {
					mi = i;
					over = sum + n->strength + n->production - 255;
				}
				continue;
			}
			if (sum > 255) {
				if (sum - 255 < over) {
					mi = i;
					over = sum - 255;
				}
				continue;
			}
			
			return t;

			i++;
		}

		return adjacentTiles[mi];
	}

	void computeMoves(std::set<hlt::Move>& moves, bool debug = false, std::ostream& out = std::cout) {
		std::vector<Tile*> tilesForMove = m_ownTiles;
		if (m_expansion) {
			setMoveForZeroStrengthTiles(tilesForMove);
		} else {
			setMoveForSmallStrengthTiles(tilesForMove, 8);
		}
		// remove all STILL tiles
		tilesForMove.erase(std::remove_if(tilesForMove.begin(), tilesForMove.end(), [](Tile* x) {
			return x->move == 0;
		}), tilesForMove.end());
		// order by strength
		sort(tilesForMove.begin(), tilesForMove.end(), [](Tile* a, Tile* b) {
			return a->strength > b->strength;
		});

		size_t counter = 0;
		bool last = false;
		while (!tilesForMove.empty()) {
			Tile* start = tilesForMove[0];
			tilesForMove.erase(tilesForMove.begin());
			if (start->strength == 0 || (!m_expansion && start->strength <= 8*start->production)) continue; // dont move empty and small strength tiles (maybe queued because of other releases)

			if (debug && FULLDEBUG) out << *start << std::endl;

			std::vector<AdjacentTile> adjacentTiles = m_djikstraSearch[start->id].getAdjacentTiles(m_movePenalty, m_id, debug, out);
			if (adjacentTiles.size() != 0) {
				AdjacentTile bestAdjacentTile = getBestAdjacentTile(adjacentTiles);

				PathSearch best(start, bestAdjacentTile, m_paths);

				if (debug && FULLDEBUG) {
					out << "path search: " << std::endl;
					best.print(out);
				}

				std::vector<Tile*> released(0);
				bool update = best.update(released, debug, out);

				// first tile maybe move, must be after update
				if (best.m_turns == best.m_moves) { // dont wait if turns == moves
					setMoveDirection(best.m_target.m_path[0], best.m_target.m_path[1]);
				}

				if (update) {
					// insert into move vector
					sort(released.begin(), released.end(), [](Tile* a, Tile* b) {
						return a->strength > b->strength;
					});
					tilesForMove.insert(tilesForMove.end(), released.begin(), released.end());

					m_paths.push_back(best);
				}
			} else {
				active = true;
			}

			if (debug && FULLDEBUG) {
				printPaths(out);
				out << "tiles for move: " << std::endl;
				for (Tile* t : tilesForMove) {
					out << *t << std::endl;
				}
				out << std::endl;
			}

			counter++;

			// once again
			if (!last && tilesForMove.empty()) {
				std::vector<Tile*> tiles = m_ownTiles;
				if (m_expansion) {
					setMoveForZeroStrengthTiles(tiles);
				} else {
					setMoveForSmallStrengthTiles(tiles, 8);
				}
				// remove all STILL tiles
				tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [](Tile* x) {
					return x->strength == 0;
				}), tiles.end());
				// order by strength
				sort(tiles.begin(), tiles.end(), [](Tile* a, Tile* b) {
					return a->strength > b->strength;
				});

				tilesForMove.insert(tilesForMove.begin(), tiles.begin(), tiles.end());
				last = true;
			}

			if (m_timer.timeCheck()) {
				if (debug) out << counter << std::endl << "TIME IS UP!" << std::endl;

				if (counter == 1) active = true; // dijkstra needs to much time, use OverkillBotExtended

				break;
			}
		}

		// set moves for response
		for (Tile* t : m_ownTiles) {
			moves.insert({ { t->x, t->y }, (unsigned char)(t->move == -1 ? STILL : t->move) });
		}
		if (debug) out << m_ownTiles.size() << " / " << m_timer << std::endl;
	}

	void printOwnTiles(std::ostream& out) {
		out << "own tiles: " << std::endl;
		printTiles(m_ownTiles, out);
	}
	void printTiles(std::vector<Tile*> tiles, std::ostream& out) {
		for (Tile* t : tiles) {
			out << *t << std::endl;
		}
	}
	void printMap(std::ostream& out) {
		out << "   ";
		for (size_t j = 0; j < m_width; j++) {
			out << "| " << std::setw(2) << j << " ";
		}
		out << "|" << std::endl;
		out << "---";
		for (size_t j = 0; j < m_width; j++) {
			out << "-----";
		}
		out << "-" << std::endl;
		for (size_t i = 0; i < m_height; i++) {
			out << "   ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(4) << (int)m_gameMap[i][j].id;
			}
			out << "|" << std::endl;

			out << std::setw(2) << i << " ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(1) << (int)m_gameMap[i][j].owner << "/" << std::setw(2) << (int)m_gameMap[i][j].production;
			}
			out << "|" << std::endl;

			out << "   ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(4) << (int)m_gameMap[i][j].strength;
			}
			out << "|" << std::endl;

			out << "---";
			for (size_t j = 0; j < m_width; j++) {
				out << "-----";
			}
			out << "-" << std::endl;
		}
	}
	void printPaths(std::ostream& out) {
		out << "global paths: " << std::endl;
		for (PathSearch p : m_paths) {
			p.print(out);
		}
	}
	friend std::ostream& operator<<(std::ostream& os, const GameState& state) {
		//os << "    ";
		//for (size_t j = 0; j < state.m_width; j++) {
		//	os << "| " << std::setw(5) << j << " ";
		//}
		//os << "|" << std::endl;
		//os << "   ";
		//for (size_t j = 0; j < state.m_width; j++) {
		//	os << "--------";
		//}
		//os << "-" << std::endl;
		//for (size_t i = 0; i < state.m_height; i++) {
		//	os << std::setw(3) << i << " ";
		//	for (size_t j = 0; j < state.m_width; j++) {
		//		os << "| " << std::setw(5) << std::fixed << std::setprecision(2) << state.m_gameMap[i][j].sDivP << " ";
		//	}
		//	os << "|" << std::endl;
		//	os << "   ";
		//	for (size_t j = 0; j < state.m_width; j++) {
		//		os << "--------";
		//	}
		//	os << "-" << std::endl;
		//}

		// num

		os << "   ";
		for (size_t j = 0; j < state.m_width; j++) {
			os << "| " << std::setw(3) << j << " ";
		}
		os << "|" << std::endl;
		os << "   ";
		for (size_t j = 0; j < state.m_width; j++) {
			os << "------";
		}
		os << "-" << std::endl;
		for (size_t i = 0; i < state.m_height; i++) {
			os << std::setw(2) << i << " ";
			for (size_t j = 0; j < state.m_width; j++) {
				os << "| " << std::setw(3) << (int)state.m_gameMap[i][j].owner << " ";
				//os << "| " << std::setw(3) << (int)state.m_gameMap[i][j].strength << " ";
			}
			os << "|" << std::endl;

			os << "   ";
			for (size_t j = 0; j < state.m_width; j++) {
				os << "------";
			}
			os << "-" << std::endl;
		}
		return os;
	}
};

class OverkillBotExtended {
public:
	std::vector< std::vector<Tile> > m_gameMap;
	unsigned char m_height;
	unsigned char m_width;
	unsigned char m_id;
	unsigned char m_initialPlayers;
	unsigned int m_territorySize[7] = { 0 };
	Timer m_timer;
	std::vector<Tile*> m_ownTiles;

	OverkillBotExtended(const hlt::GameMap& gameMap, unsigned char myId, bool debug = false, std::ostream& out = std::cout) : m_width((unsigned char)gameMap.width), m_height((unsigned char)gameMap.height), m_id(myId), m_timer()
	{
		m_timer.startTimer(950);
		m_gameMap = std::vector< std::vector<Tile> >(m_height, std::vector<Tile>(m_width));
		for (unsigned char y = 0; y < m_height; y++) {
			for (unsigned char x = 0; x < m_width; x++) {
				const hlt::Site& s = gameMap.contents[y][x];
				m_gameMap[y][x] = Tile(x, y, (unsigned char)s.owner, (unsigned char)s.strength, (unsigned char)s.production, m_width);

				for (size_t i = 0; i < 4; i++) {
					m_gameMap[y][x].neighbours[i] = getTile(&m_gameMap[y][x], CARDINALS[i]);
				}
			}
		}
		computeTerritorySize();
		m_initialPlayers = computePlayers();
		m_ownTiles = getPlayerTiles(m_id);

		if (debug) out << " Init OBE: " << m_timer << std::endl;

		if (debug && FULLDEBUG) {
			printMap(out);
		}
	}
	void computeTerritorySize() {
		for (size_t i = 0; i < 7; i++) {
			m_territorySize[i] = 0;
		}

		for (unsigned short i = 0; i < m_height; i++) {
			for (unsigned short j = 0; j < m_width; j++) {
				m_territorySize[m_gameMap[i][j].owner] += 1;
			}
		}
	}
	unsigned char computePlayers() {
		unsigned char num = 0;
		for (size_t i = 0; i < 7; i++) {
			if (m_territorySize[i]) {
				num += 1;
			}
		}
		return num;
	}
	Tile* getTile(Tile* t, unsigned char direction = STILL) {
		unsigned char x = t->x, y = t->y;
		if (direction != STILL) {
			if (direction == NORTH) {
				if (y == 0) y = m_height - 1;
				else y--;
			} else if (direction == EAST) {
				if (x == m_width - 1) x = 0;
				else x++;
			} else if (direction == SOUTH) {
				if (y == m_height - 1) y = 0;
				else y++;
			} else if (direction == WEST) {
				if (x == 0) x = m_width - 1;
				else x--;
			}
		}
		return &m_gameMap[y][x];
	}
	std::vector<Tile*> getPlayerTiles(unsigned char id) {
		std::vector<Tile*> tiles(0);
		size_t k = 0;
		for (unsigned short y = 0; y < m_height; y++) {
			for (unsigned short x = 0; x < m_width; x++) {
				if (m_gameMap[y][x].owner == id) {
					tiles.push_back(&m_gameMap[y][x]);
				}
			}
		}

		sort(tiles.begin(), tiles.end(), [](Tile* a, Tile* b) {
			return a->id < b->id;
		});

		return tiles;
	}
	void setMoveDirection(Tile* t, Tile* next) {
		for (unsigned char c : CARDINALS) {
			Tile* n = getTile(t, c);
			if (n->id == next->id) {
				t->move = c;
				break;
			}
		}
	}
	bool isBorder(Tile* t) {
		for (Tile* n : t->neighbours) {
			if (n->owner != m_id) return true;
		}
		return false;
	}

	unsigned char findNearestEnemyDirection(Tile* t) {
		unsigned char direction = NORTH;
		unsigned char maxDistance = (std::min)(m_width, m_height) / 2;

		for (unsigned char c : CARDINALS) {
			unsigned char dist = 0;
			Tile* current = t;
			while (current->owner == m_id && dist < maxDistance) {
				dist++;
				current = current->neighbours[c-1];
			}

			if (dist < maxDistance) {
				direction = c;
				maxDistance = dist;
			}
		}

		return direction;
	}
	float heuristic(Tile* t, unsigned char str) {
		if (t->owner == 0 && t->strength > 0) {
			return (float)t->production / (float)t->strength;
		} else {
			unsigned short damage = 0;
			for (Tile* n : t->neighbours) {
				if (n->owner != 0 && n->owner != m_id) {
					damage += (std::min)(str, n->strength);
				}
			}
			return damage;
		}
	}
	void move(Tile* t, bool debug = false, std::ostream& out = std::cout) {
		float damage = -1;
		Tile* target = nullptr;

		for (Tile* n : t->neighbours) {
			if (n->owner != m_id) {
				float d = heuristic(n, t->strength);
				if (d > damage) {
					damage = d;
					target = n;
				}
			}
		}

		if (debug) out << *t;
		if (debug && target != nullptr) out << " target:" << *target;
		if (debug) out << std::endl;

		if (target != nullptr && target->strength < t->strength) {
			setMoveDirection(t, target);
			return;
		}

		if (t->strength < (t->production * 5)) {
			t->move = STILL;
			return;
		}

		// if the cell isn't on the border
		if (!isBorder(t)) {
			t->move = findNearestEnemyDirection(t);
			return;
		}

		// otherwise wait until you can attack
		t->move = STILL;
		return;
	}

	void computeMoves(std::set<hlt::Move>& moves, bool debug = false, std::ostream& out = std::cout) {
		std::vector<Tile*> tilesForMove = m_ownTiles;
		sort(tilesForMove.begin(), tilesForMove.end(), [](Tile* a, Tile* b) {
			return a->strength > b->strength;
		});

		for (Tile* t : tilesForMove) {
			move(t, debug, out);

			if (m_timer.timeCheck()) {
				if (debug) out << "OBE TIME IS UP!" << std::endl;
				break;
			}
		}

		// set moves for response
		for (Tile* t : m_ownTiles) {
			moves.insert({ { t->x, t->y }, (unsigned char)(t->move == -1 ? STILL : t->move) });
		}

		if (debug) out << m_ownTiles.size() << " / " << m_timer << std::endl;
	}

	void printMap(std::ostream& out) {
		out << "   ";
		for (size_t j = 0; j < m_width; j++) {
			out << "| " << std::setw(2) << j << " ";
		}
		out << "|" << std::endl;
		out << "---";
		for (size_t j = 0; j < m_width; j++) {
			out << "-----";
		}
		out << "-" << std::endl;
		for (size_t i = 0; i < m_height; i++) {
			out << "   ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(4) << (int)m_gameMap[i][j].id;
			}
			out << "|" << std::endl;

			out << std::setw(2) << i << " ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(1) << (int)m_gameMap[i][j].owner << "/" << std::setw(2) << (int)m_gameMap[i][j].production;
			}
			out << "|" << std::endl;

			out << "   ";
			for (size_t j = 0; j < m_width; j++) {
				out << "|" << std::setw(4) << (int)m_gameMap[i][j].strength;
			}
			out << "|" << std::endl;

			out << "---";
			for (size_t j = 0; j < m_width; j++) {
				out << "-----";
			}
			out << "-" << std::endl;
		}
	}
};

int main() {
    std::cout.sync_with_stdio(0);

    unsigned char myId;
    hlt::GameMap presentMap;
    getInit(myId, presentMap);
	GameState gameState(presentMap, myId);

    sendInit("MyC++Bot");

#ifdef DEBUG
	std::ofstream debugFile;
	debugFile.open("debugOutput.dat");
	if (!debugFile.is_open()) throw std::runtime_error("Could not open file for debug ouput");
#endif
    std::set<hlt::Move> moves;
	unsigned short frame = 0;
    while(true) {
        moves.clear();
        getFrame(presentMap);
		
#ifdef DEBUG
		debugFile << "frame: " << frame << std::endl;
		if (active) {
			OverkillBotExtended obe(presentMap, myId, true, debugFile);
			obe.computeMoves(moves, true, debugFile);
		} else {
			gameState.updateGameMap(presentMap, true, debugFile);
			gameState.computeMoves(moves, true, debugFile);
		}
		debugFile.flush();
#else
		if (active) {
			OverkillBotExtended obe(presentMap, myId);
			obe.computeMoves(moves);
		} else {
			gameState.updateGameMap(presentMap);
			gameState.computeMoves(moves);
		}
#endif

		frame++;
		sendFrame(moves);
    }

#ifdef DEBUG
	debugFile.close();
#endif
    return 0;
}

