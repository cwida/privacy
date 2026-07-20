SELECT
    avg(l_quantity) AS avg_qty,
    avg(l_extendedprice) AS avg_price,
    avg(l_discount) AS avg_disc
FROM
    lineitem
WHERE
    l_shipdate <= CAST('1998-09-02' AS date);
