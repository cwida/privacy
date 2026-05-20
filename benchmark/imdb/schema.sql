CREATE TABLE aka_name (
	id INTEGER NOT NULL,
	person_id INTEGER NOT NULL,
	name TEXT NOT NULL,
	imdb_index TEXT,
	name_pcode_cf TEXT,
	name_pcode_nf TEXT,
	surname_pcode TEXT,
	md5sum TEXT
);

CREATE TABLE aka_title (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	title TEXT NOT NULL,
	imdb_index TEXT,
	kind_id INTEGER NOT NULL,
	production_year INTEGER,
	phonetic_code TEXT,
	episode_of_id INTEGER,
	season_nr INTEGER,
	episode_nr INTEGER,
	note TEXT,
	md5sum TEXT
);

CREATE TABLE cast_info (
	id INTEGER NOT NULL,
	person_id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	person_role_id INTEGER,
	note TEXT,
	nr_order INTEGER,
	role_id INTEGER NOT NULL
);

CREATE TABLE char_name (
	id INTEGER NOT NULL,
	name TEXT NOT NULL,
	imdb_index TEXT,
	imdb_id INTEGER,
	name_pcode_nf TEXT,
	surname_pcode TEXT,
	md5sum TEXT
);

CREATE TABLE comp_cast_type (
	id INTEGER NOT NULL,
	kind TEXT NOT NULL
);

CREATE TABLE company_name (
	id INTEGER NOT NULL,
	name TEXT NOT NULL,
	country_code TEXT,
	imdb_id INTEGER,
	name_pcode_nf TEXT,
	name_pcode_sf TEXT,
	md5sum TEXT
);

CREATE TABLE company_type (
	id INTEGER NOT NULL,
	kind TEXT NOT NULL
);

CREATE TABLE complete_cast (
	id INTEGER NOT NULL,
	movie_id INTEGER,
	subject_id INTEGER NOT NULL,
	status_id INTEGER NOT NULL
);

CREATE TABLE info_type (
	id INTEGER NOT NULL,
	info TEXT NOT NULL
);

CREATE TABLE keyword (
	id INTEGER NOT NULL,
	keyword TEXT NOT NULL,
	phonetic_code TEXT
);

CREATE TABLE kind_type (
	id INTEGER NOT NULL,
	kind TEXT NOT NULL
);

CREATE TABLE link_type (
	id INTEGER NOT NULL,
	link TEXT NOT NULL
);

CREATE TABLE movie_companies (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	company_id INTEGER NOT NULL,
	company_type_id INTEGER NOT NULL,
	note TEXT
);

CREATE TABLE movie_info (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	info_type_id INTEGER NOT NULL,
	info TEXT NOT NULL,
	note TEXT
);

CREATE TABLE movie_info_idx (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	info_type_id INTEGER NOT NULL,
	info TEXT NOT NULL,
	note TEXT
);

CREATE TABLE movie_keyword (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	keyword_id INTEGER NOT NULL
);

CREATE TABLE movie_link (
	id INTEGER NOT NULL,
	movie_id INTEGER NOT NULL,
	linked_movie_id INTEGER NOT NULL,
	link_type_id INTEGER NOT NULL
);

CREATE TABLE name (
	id INTEGER NOT NULL,
	name TEXT NOT NULL,
	imdb_index TEXT,
	imdb_id INTEGER,
	gender TEXT,
	name_pcode_cf TEXT,
	name_pcode_nf TEXT,
	surname_pcode TEXT,
	md5sum TEXT
);

CREATE TABLE person_info (
	id INTEGER NOT NULL,
	person_id INTEGER NOT NULL,
	info_type_id INTEGER NOT NULL,
	info TEXT NOT NULL,
	note TEXT
);

CREATE TABLE role_type (
	id INTEGER NOT NULL,
	role TEXT NOT NULL
);

CREATE TABLE title (
	id INTEGER NOT NULL,
	title TEXT NOT NULL,
	imdb_index TEXT,
	kind_id INTEGER NOT NULL,
	production_year INTEGER,
	imdb_id INTEGER,
	phonetic_code TEXT,
	episode_of_id INTEGER,
	season_nr INTEGER,
	episode_nr INTEGER,
	series_years TEXT,
	md5sum TEXT
);
