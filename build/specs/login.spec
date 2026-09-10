# Login Tests

tags: login, smoke

## Successful login

* Navigate to login page
* Enter username "admin"
* Enter password "password123"
* Click login button
* Verify welcome message "Welcome, admin!"

## Failed login

* Navigate to login page
* Enter username "admin"
* Enter password "wrong"
* Click login button
* Verify error message "Invalid credentials"
