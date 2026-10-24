from pyspark.sql import functions as F
import sys
import json
import re
from pyspark.sql import SparkSession
from pyspark.sql.window import Window
from pyspark.sql.functions import col, rank, count, first, sum as spark_sum

def _fmt(v):
    """Convert one cell value to the text that is printed."""
    if v is None:
        return "NULL"
    if isinstance(v, float):
        return f"{v:.4f}"
    return str(v)

def print_df(task_name, df, sort_cols):
    """Print a deterministically sorted DataFrame for autograding."""
    ordered = df.orderBy(*sort_cols)
    print(f"BEGIN_{task_name}")
    print("\t".join(ordered.columns))
    for row in ordered.toLocalIterator():
        print("\t".join(_fmt(v) for v in row))
    print(f"END_{task_name}")
    print()  # empty line between blocks, for readability


def main():

    # TASK 1 RDD
    spark = SparkSession.builder \
        .appName("NewsArticlesRDDOnly") \
        .getOrCreate()

    sc = spark.sparkContext
    spark.sparkContext.setLogLevel('ERROR')
    sc.setLogLevel('ERROR')

    places = {"delhi", "mumbai", "kolkata", "chennai", "bengaluru", "hyderabad", "lucknow", "srinagar","kashmir", "punjab", "gujarat", "maharashtra", "kerala", "karnataka", "bengal", "assam", "bihar", "rajasthan", "pakistan", "china", "noida", "mathura"}
    broadcast_places = sc.broadcast(places)

    json_dir_path = sys.argv[1]
    files_rdd = sc.wholeTextFiles(json_dir_path)

    def process_article(file_record):
        file_path, content = file_record
        results = []
        
        try:
            data = json.loads(content)
            
            article_id = data.get("article_id")
            article_body = data.get("article_body")

            # if article_id == "ISRO, Sivan, GSAT-11,":
            #     print(f"Erroneous file path: {file_path}")
            
            if not article_id or not article_body:
                return results
                
            words = re.findall(r'[a-z]+', article_body.lower())
            valid_places = broadcast_places.value
            
            for word in words:
                if word in valid_places:
                    results.append((word, article_id))
                    
        except Exception as e:
            pass
            
        return results

    pairs_rdd = files_rdd.flatMap(process_article)

    distinct_rdd = pairs_rdd.distinct()

    sorted_rdd = distinct_rdd.sortBy(lambda pair: (pair[0], pair[1]))
    

    print("BEGIN_TASK1_RDD")
    print(sorted_rdd.count())
    for place, article_id in sorted_rdd.take(20 if sorted_rdd.count() > 20 else sorted_rdd.count()):
        print(f"{place}\t{article_id}")
    print("END_TASK1_RDD")
    print()

    # TASK 2

    distinct_df = spark.createDataFrame(distinct_rdd, ["place", "article_id"])
    print_df('TASK2_PAIRS', distinct_df.orderBy('place', 'article_id').limit(20),
[F.col('place').asc(), F.col('article_id').asc()])

    # TASK 3

    # using pairs_rdd to create dataFrame
    pairs_df = spark.createDataFrame(pairs_rdd, ["place", "article_id"])
    term_frequency_df = pairs_df.groupBy("article_id", "place").count().withColumnRenamed("count", "term_frequency")
    print_df('TASK3_FREQUENCIES', term_frequency_df.orderBy('article_id', 'place', 'term_frequency'), [F.col('article_id').asc(), F.col('place').asc()])

    # compute frequency using pairs_df and dataframe functions
    window_spec = Window.partitionBy("article_id").orderBy(col("term_frequency").desc())

    dominant_places_df = term_frequency_df.withColumn("rank", rank().over(window_spec)).filter(col("rank") == 1).drop("rank")
    print_df('TASK3_DOMINANT', dominant_places_df.orderBy('article_id', 'place', 'term_frequency'), [F.col('article_id').asc(), F.col('place').asc()])

    # TASK 4
    blind_spot_df = distinct_df.groupBy("place").agg(count("article_id").alias("article_count"),first("article_id").alias("article_id")).filter(col("article_count") == 1).select("place", "article_id")
    print_df('TASK4_BLIND_SPOTS', blind_spot_df, [F.col('place').asc(), F.col('article_id').asc()])

    # TASK 5
    place_totals = distinct_df.groupBy("place").agg(count("article_id").alias("total_articles"))

    df1 = distinct_df.withColumnRenamed("place", "p1")
    df2 = distinct_df.withColumnRenamed("place", "p2")

    cooccur_pairs = df1.join(df2, "article_id").filter(col("p1") < col("p2")).groupBy("p1", "p2").agg(count("article_id").alias("intersection_count"))

    # join back with place totals to get |A| and |B| for each pair
    step1 = cooccur_pairs.join(
        place_totals.withColumnRenamed("place", "p1").withColumnRenamed("total_articles", "total_p1"), 
        "p1"
    )
    pairs_with_totals = step1.join(
        place_totals.withColumnRenamed("place", "p2").withColumnRenamed("total_articles", "total_p2"), 
        "p2"
    )

    # compute Jaccard Similarity
    
    jaccard_df = pairs_with_totals \
        .withColumn("union_count", col("total_p1") + col("total_p2") - col("intersection_count")) \
        .withColumn("jaccard", col("intersection_count") / col("union_count"))

    top_5_df = jaccard_df.filter(col("intersection_count") > 0).sort(
        col("jaccard").desc(), 
        col("p1").asc(), 
        col("p2").asc()
    ).limit(5)

    print_top_5_df = top_5_df.withColumnRenamed("intersection_count", "cooccurrence_count").select("p1", "p2", "jaccard", "cooccurrence_count")

    bottom_5_df = jaccard_df.filter(col("intersection_count") > 0).sort(
        col("jaccard").asc(), 
        col("p1").asc(), 
        col("p2").asc()
    ).limit(5)

    print_bottom_5_df = bottom_5_df.withColumnRenamed("intersection_count", "cooccurrence_count").select("p1", "p2", "jaccard", "cooccurrence_count")
    
    print_df('TASK5_TOP5', print_top_5_df, [F.col('jaccard').desc(), F.col('p1').asc(), F.col('p2').asc()])
    print_df('TASK5_BOTTOM5', print_bottom_5_df, [F.col('jaccard').asc(), F.col('p1').asc(), F.col('p2').asc()])

    # TASK 6

    top_5_places_df = term_frequency_df \
    .groupBy("place") \
    .agg(
        spark_sum("term_frequency").alias("total_mentions"),
        count("article_id").alias("article_count")
    ) \
    .orderBy(F.col('total_mentions').desc(), F.col('place').asc()).select("place", "total_mentions", "article_count").limit(5)

    print_df('TASK6_TOP5', top_5_places_df, [F.col('total_mentions').desc(), F.col('place').asc()])

    # TASK 7

    top_10_articles_df = term_frequency_df \
    .groupBy("article_id") \
    .agg(
        count("place").alias("distinct_places_count")
    ) \
    .orderBy(
        col("distinct_places_count").desc(),
        col("article_id").asc()
    ) \
    .limit(10)

    print_df('TASK7_TOP10', top_10_articles_df, [col('distinct_places_count').desc(), col('article_id').asc()])
    
if __name__ == "__main__":
    main()