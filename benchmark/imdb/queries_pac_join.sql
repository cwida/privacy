SELECT t.kind_id, COUNT(*) AS c FROM cast_info AS c JOIN title AS t ON c.movie_id = t.id GROUP BY t.kind_id ORDER BY c DESC LIMIT 20;
SELECT t.production_year, COUNT(*) AS c FROM cast_info AS c JOIN title AS t ON c.movie_id = t.id WHERE t.production_year IS NOT NULL GROUP BY t.production_year ORDER BY c DESC LIMIT 20;
SELECT c.role_id, t.kind_id, COUNT(*) AS c FROM cast_info AS c JOIN title AS t ON c.movie_id = t.id GROUP BY c.role_id, t.kind_id ORDER BY c DESC LIMIT 20;
