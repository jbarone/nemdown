# Documentation Example with Mermaid Diagrams

This document contains several examples of using **Mermaid.js** syntax within Markdown fenced code blocks to create dynamic, text-based visualizations.

---

## 1. Flowchart
This flowchart maps out a simple decision-making process for an automated build pipeline.

```mermaid
flowchart TD
    A[Start: Code Push] --> B{Run Tests?}
    B -- Yes --> C[Execute Unit Tests]
    B -- No --> E[Deploy to Staging]
    C --> D{Tests Pass?}
    D -- Yes --> E
    D -- No --> F[Send Alert & Halt]
    E --> G[Production Release]
```

---

## 2. Sequence Diagram
This diagram outlines the communication between a Client, an API Gateway, and a Database during a standard authentication request.

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant Gateway as API Gateway
    participant DB as Database

    Client->>Gateway: POST /auth/login (Credentials)
    activate Gateway
    Gateway->>DB: Query User Records
    activate DB
    DB-->>Gateway: Return User Object & Hash
    deactivate DB

    alt Credentials Valid
        Gateway-->>Client: 200 OK (JWT Token)
    else Credentials Invalid
        Gateway-->>Client: 401 Unauthorized
    end
    deactivate Gateway
```

---

## 3. State Diagram
This diagram represents the lifecycles and transitions of an online shopping order.

```mermaid
stateDiagram-v2
    [*] --> Placed
    Placed --> Paid : Payment Success
    Placed --> Cancelled : Payment Timeout
    Paid --> Shipped : Fulfillment
    Shipped --> Delivered : Carrier Confirmation
    Delivered --> [*]
    Cancelled --> [*]
```

---

## 4. Class Diagram
This object-oriented structure maps out the relationships (inheritance and composition) between a Bank Account base class, a Checking Account subclass, and a Customer entity.

```mermaid
classDiagram
    note for BankAccount "Base class for all accounts"

    class Customer {
        +String name
        +String email
        +addAccount(BankAccount account)
    }

    class BankAccount {
        #String accountNumber
        #double balance
        +deposit(double amount) void
        +withdraw(double amount) bool*
    }

    class CheckingAccount {
        +double overdraftLimit
        +withdraw(double amount) bool
    }

    %% Relationships
    Customer "1" *-- "many" BankAccount : owns
    BankAccount <|-- CheckingAccount : inherits
```
