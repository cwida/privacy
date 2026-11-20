SELECT
    avg(l_quantity) AS avg_qty
FROM
    lineitem
WHERE
    l_shipdate <= CAST('1998-09-02' AS date);
