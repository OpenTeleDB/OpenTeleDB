/* Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd */
/* contrib/query_check/query_check--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION query_check" to load this file. \quit

CREATE FUNCTION load_query_check() RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'load_dynamic_library'
LANGUAGE C IMMUTABLE STRICT