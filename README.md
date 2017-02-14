# Halite
Artificial intelligence programming challenge (https://halite.io/)

Finished in Diamond League (Top 24).

Some bot features:
- Dijkstra search for every tile (costs = production), to get minimum production lost paths to all adjacent tiles, time consuming for large maps
- Next target for every tile is computed with respect to strength/production, move penalty for every step (average production of all own tiles) and the actual production lost on the way to the target (because of the movement)
- A movement is triggered if the sum of strengths of a path is greater than the target strength (problems in combat mode with zero strength tiles -> only move tiles with strength > 8 * production)
- Switch to Ovekillbot (http://forums.halite.io/t/so-youve-improved-the-random-bot-now-what/482) if Dijkstra search needs to much time, in most cases its already a winning position

Improvments:
-   Many, many things. The most important one: Use Overkill far more effective.