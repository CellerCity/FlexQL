CREATE DATABASE automated_test_db;
USE automated_test_db;

CREATE TABLE employees (ID INT PRIMARY, NAME VARCHAR, SALARY DECIMAL);

INSERT INTO employees VALUES (101, 'Ada_Lovelace', 120500.50);
INSERT INTO employees VALUES (102, 'Alan_Turing', 115000.00);
INSERT INTO employees VALUES (103, 'Grace_Hopper', 130250.75);

SELECT * FROM employees WHERE ID = 102;
SELECT * FROM employees WHERE ID = 101;

DROP TABLE employees;
DROP DATABASE automated_test_db;

.exit