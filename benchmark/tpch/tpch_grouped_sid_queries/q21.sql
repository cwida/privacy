SELECT l.sample_id, l.s_name, count(*) AS numwait
FROM benchmark_lineitem_worlds l
WHERE l.is_late
  AND l.is_orderstatus_f
  AND l.is_nation_saudi_arabia
  AND EXISTS (SELECT 1 FROM lineitem_enhanced l2
              WHERE l2.l_orderkey = l.l_orderkey
                AND l2.l_suppkey <> l.l_suppkey)
  AND NOT EXISTS (SELECT 1 FROM lineitem_enhanced l3
                  WHERE l3.l_orderkey = l.l_orderkey
                    AND l3.l_suppkey <> l.l_suppkey
                    AND l3.is_late)
GROUP BY l.sample_id, l.s_name
ORDER BY l.sample_id, numwait DESC, l.s_name;
