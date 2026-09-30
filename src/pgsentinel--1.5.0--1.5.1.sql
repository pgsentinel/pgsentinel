/* pgsentinel--1.5.0--1.5.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgsentinel UPDATE TO '1.5.1'" to load this file. \quit

-- get_parsedinfo() returns every backend's query text, literals included.
REVOKE EXECUTE ON FUNCTION get_parsedinfo(int) FROM PUBLIC;
