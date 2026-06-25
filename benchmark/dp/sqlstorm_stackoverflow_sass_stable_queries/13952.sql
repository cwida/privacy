WITH PostDetails AS (
    SELECT
        p.Id AS PostId,
        p.Title,
        p.CreationDate,
        p.ViewCount,
        p.Score,
        u.Reputation AS OwnerReputation,
        t.TagName
    FROM
        Posts p
    JOIN
        Users u ON p.OwnerUserId = u.Id
    LEFT JOIN
        LATERAL (SELECT unnest(string_to_array(p.Tags, '>')) AS TagName) t
    ON true
)

SELECT
    pd.PostId,
    pd.Title,
    pd.CreationDate,
    pd.ViewCount,
    pd.Score,
    pd.OwnerReputation,
    COUNT(pd.TagName) AS TagCount
FROM
    PostDetails pd
GROUP BY
    pd.PostId, pd.Title, pd.CreationDate, pd.ViewCount, pd.Score, pd.OwnerReputation
ORDER BY
    pd.Score DESC, pd.ViewCount DESC;